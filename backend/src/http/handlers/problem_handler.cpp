// =============================================================================
//  problem_handler.cpp —— GET /api/problems 实现
//  SPEC §5.2.2 / §3.3.5 G / §2.2.2
// =============================================================================

#include "http/handlers/problem_handler.hpp"

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "common/error_code.hpp"
#include "http/HttpServer.hpp"  // 完整定义（register_problem_routes 用到）

namespace oj::http::handlers {

namespace {

using nlohmann::json;

// ProblemListItem → JSON
//
// 字段映射（与 SPEC §3.3.5 G "每条目显示" + §2.2.2 对齐）：
//   id, title, difficulty, tags
//   created_at                       (前端可能不需要但保留)
//   stats:
//     total, accepted, pass_rate     (admin 看 breakdown；游客只看 pass_rate ——
//                                       但 API 一次给全，前端决定显示哪些)
//   is_published                     (内部用)
json item_to_json(const oj::domain::ProblemListItem& it) {
    json tags = json::array();
    for (const auto& t : it.tags) {
        tags.push_back({
            {"id",   t.id},
            {"name", t.name},
            {"slug", t.slug},
        });
    }
    return json{
        {"id",           it.id},
        {"title",        it.title},
        {"difficulty",   oj::domain::to_string(it.difficulty)},
        {"is_published", it.is_published},
        {"created_by",   it.created_by},
        {"created_at",   it.created_at},
        {"tags",         tags},
        {"stats",        {
            {"total",     it.total_submissions},
            {"accepted",  it.accepted_submissions},
            {"pass_rate", it.pass_rate()},
        }},
    };
}

json result_to_json(const oj::domain::ProblemListResult& r) {
    json items = json::array();
    for (const auto& it : r.items) items.push_back(item_to_json(it));
    return json{
        {"items", items},
        {"total", r.total},
        {"page",  r.page},
        {"size",  r.page_size},
    };
}

// ---------------------------------------------------------------------------
//  GET /api/tags
//
//  业务流程：
//   1) DB readiness → 503 if down
//   2) service.list_tags() → vector<Tag>（id ASC）
//   3) 序列化为 JSON → write_ok（data 为数组，envelope 同 §5.1）
//
//  响应 data 形状：
//   [
//     {"id": 1, "name": "数组", "slug": "数组"},
//     {"id": 2, "name": "字符串", "slug": "string"},
//     ...
//   ]
// ---------------------------------------------------------------------------
json tags_to_json(const std::vector<oj::domain::Tag>& tags) {
    json arr = json::array();
    for (const auto& t : tags) {
        arr.push_back({
            {"id",   t.id},
            {"name", t.name},
            {"slug", t.slug},
        });
    }
    return arr;
}

void handle_tags(const std::shared_ptr<oj::domain::IProblemService>& service,
                 const std::function<bool()>& is_db_ready,
                 const httplib::Request&, httplib::Response& res) {
    using oj::common::ErrorCode;

    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    std::vector<oj::domain::Tag> tags;
    try {
        tags = service->list_tags();
    } catch (const std::exception& e) {
        spdlog::error("GET /api/tags DB error: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    spdlog::info("GET /api/tags returned={}", tags.size());
    write_ok(res, tags_to_json(tags));
}

// 共享的 GET /api/problems handler 闭包
//
// 业务流程：
//   1) DB readiness check → 503 if down
//   2) 解析 URL query → ProblemListQuery（带校验）
//   3) 校验失败 → 1001 BadRequest
//   4) service.list() → ProblemListResult
//   5) 序列化为 JSON → write_ok
void handle_list(const std::shared_ptr<oj::domain::IProblemService>& service,
                 const std::function<bool()>& is_db_ready,
                 const httplib::Request& req, httplib::Response& res) {
    using oj::common::ErrorCode;

    // 1) DB 不可用 → 503
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 2) URL → Query
    auto parsed = oj::domain::parse_problems_list_query(req.params, /*is_admin=*/false);
    if (!parsed.error_message.empty()) {
        write_error(res, ErrorCode::BadRequest, parsed.error_message);
        return;
    }

    // 3) service.list —— 业务校验（page < 1 / page_size 白名单）由 service 兜底
    oj::domain::ProblemListResult result;
    try {
        result = service->list(parsed.query);
    } catch (const std::exception& e) {
        spdlog::error("GET /api/problems DB error: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 4) 返回 envelope
    spdlog::info("GET /api/problems page={} size={} total={} returned={}",
                 result.page, result.page_size, result.total, result.items.size());
    write_ok(res, result_to_json(result));
}

// ---------------------------------------------------------------------------
//  ProblemDetail → JSON
//
//  字段映射（SPEC §2.2.3 / §3.3.5 H）：
//   id, title, content_md, difficulty, tags, time_limit_ms, memory_limit_mb,
//   output_limit_mb, is_published, created_by, created_at
//   sample_testcases: [{case_index, input, expected_output, is_sample, score}]
//                     —— 隐藏点不返回（list_samples 限制）
// ---------------------------------------------------------------------------
json detail_to_json(const oj::domain::ProblemDetail& d) {
    json tags = json::array();
    for (const auto& t : d.tags) {
        tags.push_back({{"id", t.id}, {"name", t.name}, {"slug", t.slug}});
    }
    json cases = json::array();
    for (const auto& c : d.sample_testcases) {
        cases.push_back({
            {"case_index",       c.case_index},
            {"input",            c.input},
            {"expected_output",  c.expected_output},
            {"is_sample",        c.is_sample},
            {"score",            c.score},
        });
    }
    const auto& p = d.problem;
    return json{
        {"id",              p.id},
        {"title",           p.title},
        {"content_md",      p.content_md},
        {"difficulty",      oj::domain::to_string(p.difficulty)},
        {"tags",            tags},
        {"time_limit_ms",   p.time_limit_ms},
        {"memory_limit_mb", p.memory_limit_mb},
        {"output_limit_mb", p.output_limit_mb},
        {"is_published",    p.is_published},
        {"created_by",      p.created_by},
        {"created_at",      p.created_at},
        {"sample_testcases", cases},
    };
}

// GET /api/problems/:id
//
//  路径参数由 cpp-httplib 自动塞到 req.path_params（key="id"）
//  业务流程：
//   1) DB readiness → 503 if down
//   2) 解析 :id → 非法整数 / ≤ 0 → 1001 BadRequest
//   3) service.get_detail(id, include_unpublished=false) → std::nullopt = 1004
//   4) 序列化为 JSON → write_ok
void handle_detail(const std::shared_ptr<oj::domain::IProblemService>& service,
                  const std::function<bool()>& is_db_ready,
                  const httplib::Request& req, httplib::Response& res) {
    using oj::common::ErrorCode;

    // 1) DB 不可用 → 503
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 2) 解析 :id
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) {
        write_error(res, ErrorCode::BadRequest, "missing path param: id");
        return;
    }
    std::int64_t id = 0;
    try {
        id = std::stoll(it->second);
    } catch (...) {
        write_error(res, ErrorCode::BadRequest, "id must be an integer");
        return;
    }
    if (id <= 0) {
        write_error(res, ErrorCode::BadRequest, "id must be positive");
        return;
    }

    // 3) 委托 service
    std::optional<oj::domain::ProblemDetail> detail;
    try {
        detail = service->get_detail(id, /*include_unpublished=*/false);
    } catch (const std::exception& e) {
        spdlog::error("GET /api/problems/{} DB error: {}", id, e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }
    if (!detail.has_value()) {
        // 公开 API 找不到 / 未发布都走 404 —— 不暴露是否存在
        write_error(res, ErrorCode::NotFound, "problem not found");
        return;
    }

    // 4) 返回 envelope
    spdlog::info("GET /api/problems/{} ok", id);
    write_ok(res, detail_to_json(*detail));
}

}  // namespace

void register_problem_routes(HttpServer& server,
                             std::shared_ptr<oj::domain::IProblemService> service,
                             std::function<bool()> is_db_ready) {
    auto sp_service = std::move(service);
    auto sp_ready   = std::move(is_db_ready);
    server.get("/api/problems", [sp_service, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_list(sp_service, sp_ready, req, res);
    });
    server.get("/api/problems/:id", [sp_service, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_detail(sp_service, sp_ready, req, res);
    });
    server.get("/api/tags", [sp_service, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_tags(sp_service, sp_ready, req, res);
    });
}

}  // namespace oj::http::handlers

