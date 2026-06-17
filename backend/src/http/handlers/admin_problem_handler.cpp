// =============================================================================
//  admin_problem_handler.cpp —— 后台题目 CRUD + 上下架 HTTP 入口
//  SPEC §2.5 / §3.3.5 L,M / §5.2.4
// =============================================================================

#include "http/handlers/admin_problem_handler.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "common/error_code.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/submission_handler.hpp"  // parse_bearer_auth + AuthContext
#include "infra/jwt_service.hpp"

namespace oj::http::handlers {

namespace {

using nlohmann::json;
using oj::common::ErrorCode;
using oj::domain::AdminProblemError;
using oj::domain::AdminProblemErrorKind;
using oj::domain::Problem;
using oj::domain::ProblemWriteInput;
using oj::domain::Tag;
using oj::domain::Testcase;
using oj::infra::JwtService;

// ---------------------------------------------------------------------------
//  ErrorCode 翻译
// ---------------------------------------------------------------------------
ErrorCode map_admin_error(AdminProblemErrorKind k) noexcept {
    switch (k) {
        case AdminProblemErrorKind::BadRequest: return ErrorCode::BadRequest;
        case AdminProblemErrorKind::NotFound:   return ErrorCode::NotFound;
        case AdminProblemErrorKind::Internal:   return ErrorCode::Internal;
    }
    return ErrorCode::Internal;
}

// ---------------------------------------------------------------------------
//  鉴权：必须带 Bearer token 且 is_admin=true
//  缺 token / 验签失败 → 1002
//  已登录但 is_admin=false → 1003
// ---------------------------------------------------------------------------
bool require_admin(const httplib::Request& req,
                   const std::shared_ptr<JwtService>& jwt,
                   std::int64_t& out_user_id,
                   httplib::Response& res) {
    auto auth = parse_bearer_auth(req, jwt);
    if (!auth || !auth->authenticated) {
        write_error(res, ErrorCode::Unauthorized,
                    "missing or invalid access token");
        return false;
    }
    if (!auth->is_admin) {
        write_error(res, ErrorCode::Forbidden, "admin role required");
        return false;
    }
    out_user_id = auth->user_id;
    return true;
}

// ---------------------------------------------------------------------------
//  通用：DB 不可用 → 1008
// ---------------------------------------------------------------------------
bool check_db(const std::function<bool()>& is_db_ready,
              httplib::Response& res) {
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  解析 :id 路径参数
// ---------------------------------------------------------------------------
std::optional<std::int64_t> parse_id_param(const httplib::Request& req) {
    auto it = req.path_params.find("id");
    if (it == req.path_params.end()) return std::nullopt;
    try {
        std::size_t pos = 0;
        const auto& s = it->second;
        long long v = std::stoll(s, &pos);
        if (pos != s.size() || v <= 0) return std::nullopt;
        return static_cast<std::int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
//  从 JSON body 安全取字符串
// ---------------------------------------------------------------------------
std::string j_str(const json& body, const char* key) {
    auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// 从 JSON body 安全取正整数（含 0）；缺失 / 类型错 → 返回 std::nullopt
std::optional<int> j_int_opt(const json& body, const char* key) {
    auto it = body.find(key);
    if (it == body.end() || !it->is_number_integer()) return std::nullopt;
    int v = it->get<int>();
    if (v < 0) return std::nullopt;
    return v;
}

// 缺省值版本：缺失 / 类型错 → 用 fallback
int j_int_or(const json& body, const char* key, int fallback) {
    auto v = j_int_opt(body, key);
    return v.value_or(fallback);
}

// 从 JSON body 安全取 bool；缺失 → false；类型错 → 抛 BadRequest
bool j_bool(const json& body, const char* key) {
    auto it = body.find(key);
    if (it == body.end()) return false;
    if (!it->is_boolean()) {
        throw std::runtime_error(std::string{key} + " must be a boolean");
    }
    return it->get<bool>();
}

// ---------------------------------------------------------------------------
//  把 JSON body 翻译成 ProblemWriteInput
//  失败抛 std::runtime_error("msg") —— handler 转 1001
//
//  期望的 body 形态（POST / PUT 共用）：
//    {
//      "title":           string   (1..100)
//      "content_md":      string   (1..64KB)
//      "difficulty":      "easy"|"medium"|"hard"
//      "time_limit_ms":   int      [1..10000]
//      "memory_limit_mb": int      [64..1024]
//      "output_limit_mb": int      [1..256]
//      "is_published":    bool
//      "tag_ids":         [int]    (可空)
//      "cases": [
//        {
//          "case_index":      int [1..100]
//          "input":           string
//          "expected_output": string
//          "is_sample":       bool
//          "score":           int [0..100]
//        },
//        ...
//      ]   (1..100)
//    }
// ---------------------------------------------------------------------------
ProblemWriteInput parse_problem_write_input(const json& body) {
    ProblemWriteInput in;
    in.title            = j_str(body, "title");
    in.content_md       = j_str(body, "content_md");
    in.difficulty_str   = j_str(body, "difficulty");
    in.time_limit_ms    = j_int_or(body, "time_limit_ms",   2000);
    in.memory_limit_mb  = j_int_or(body, "memory_limit_mb", 256);
    in.output_limit_mb  = j_int_or(body, "output_limit_mb", 64);
    in.is_published     = j_bool(body, "is_published");

    // tag_ids —— 可空
    {
        auto it = body.find("tag_ids");
        if (it != body.end() && !it->is_null()) {
            if (!it->is_array()) {
                throw std::runtime_error("tag_ids must be an array of integers");
            }
            for (const auto& el : *it) {
                if (!el.is_number_integer()) {
                    throw std::runtime_error("tag_ids must be integers");
                }
                int v = el.get<int>();
                if (v <= 0) {
                    throw std::runtime_error("tag_ids must be positive integers");
                }
                in.tag_ids.push_back(v);
            }
        }
    }

    // cases —— 必填 1..100
    {
        auto it = body.find("cases");
        if (it == body.end() || !it->is_array()) {
            throw std::runtime_error("cases is required and must be an array");
        }
        const auto& arr = *it;
        if (arr.empty()) {
            throw std::runtime_error("cases must contain at least 1 testcase");
        }
        in.cases.reserve(arr.size());
        for (std::size_t i = 0; i < arr.size(); ++i) {
            const auto& c = arr[i];
            if (!c.is_object()) {
                throw std::runtime_error("cases[" + std::to_string(i) + "] must be an object");
            }
            Testcase tc;
            auto ci = c.find("case_index");
            if (ci == c.end() || !ci->is_number_integer()) {
                throw std::runtime_error("cases[" + std::to_string(i) +
                                         "].case_index is required and must be an integer");
            }
            tc.case_index = ci->get<int>();

            auto in_in = c.find("input");
            if (in_in == c.end() || !in_in->is_string()) {
                throw std::runtime_error("cases[" + std::to_string(i) +
                                         "].input is required and must be a string");
            }
            tc.input = in_in->get<std::string>();

            auto in_exp = c.find("expected_output");
            if (in_exp == c.end() || !in_exp->is_string()) {
                throw std::runtime_error("cases[" + std::to_string(i) +
                                         "].expected_output is required and must be a string");
            }
            tc.expected_output = in_exp->get<std::string>();

            auto in_is = c.find("is_sample");
            if (in_is != c.end() && !in_is->is_null()) {
                if (!in_is->is_boolean()) {
                    throw std::runtime_error("cases[" + std::to_string(i) +
                                             "].is_sample must be a boolean");
                }
                tc.is_sample = in_is->get<bool>();
            }

            auto in_score = c.find("score");
            if (in_score == c.end() || !in_score->is_number_integer()) {
                throw std::runtime_error("cases[" + std::to_string(i) +
                                         "].score is required and must be an integer");
            }
            tc.score = in_score->get<int>();

            in.cases.push_back(std::move(tc));
        }
    }

    return in;
}

// ---------------------------------------------------------------------------
//  Problem → JSON（创建/更新返回 data）
// ---------------------------------------------------------------------------
json problem_to_json(const Problem& p) {
    return json{
        {"id",              p.id},
        {"title",           p.title},
        {"content_md",      p.content_md},
        {"difficulty",      oj::domain::to_string(p.difficulty)},
        {"time_limit_ms",   p.time_limit_ms},
        {"memory_limit_mb", p.memory_limit_mb},
        {"output_limit_mb", p.output_limit_mb},
        {"is_published",    p.is_published},
        {"created_by",      p.created_by},
        {"created_at",      p.created_at},
    };
}

json tag_to_json(const Tag& t) {
    return json{{"id", t.id}, {"name", t.name}, {"slug", t.slug}};
}

json testcase_to_json(const Testcase& c) {
    return json{
        {"case_index",      c.case_index},
        {"input",           c.input},
        {"expected_output", c.expected_output},
        {"is_sample",       c.is_sample},
        {"score",           c.score},
    };
}

json admin_detail_to_json(const oj::domain::AdminProblemDetail& d) {
    json tags = json::array();
    for (const auto& t : d.tags) tags.push_back(tag_to_json(t));
    json cases = json::array();
    for (const auto& c : d.testcases) cases.push_back(testcase_to_json(c));
    json j = problem_to_json(d.problem);
    j["tags"]   = std::move(tags);
    j["cases"]  = std::move(cases);
    return j;
}

// 与公开 ProblemDetail 字段保持一致（list 接口要展示 created_by/created_at
// 等信息 —— admin 后台也要看到）
json list_item_to_json(const oj::domain::ProblemListItem& it) {
    json tags = json::array();
    for (const auto& t : it.tags) tags.push_back(tag_to_json(t));
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

json list_result_to_json(const oj::domain::ProblemListResult& r) {
    json items = json::array();
    for (const auto& it : r.items) items.push_back(list_item_to_json(it));
    return json{
        {"items", items},
        {"total", r.total},
        {"page",  r.page},
        {"size",  r.page_size},
    };
}

// ===========================================================================
//  路由 handlers
// ===========================================================================

// ---------------------------------------------------------------------------
//  GET /api/admin/problems?page=&size=&q=&is_published=
//  与公开列表形状一致，但 include_unpublished 始终为 true（admin 视角全可见）
//  额外支持 is_published 精确过滤（0 / 1）
// ---------------------------------------------------------------------------
void handle_list(const std::shared_ptr<oj::domain::IProblemService>& service,
                 const std::shared_ptr<JwtService>& jwt,
                 const std::function<bool()>& is_db_ready,
                 const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    // URL → ProblemListQuery（admin 入口强制 include_unpublished=true）
    auto parsed = oj::domain::parse_problems_list_query(req.params, /*is_admin=*/true);
    if (!parsed.error_message.empty()) {
        write_error(res, ErrorCode::BadRequest, parsed.error_message);
        return;
    }
    // admin 后台设计意图：默认看到全部题目（含草稿）
    // —— parse_problems_list_query 的 admin 分支只在显式 include_unpublished=1 时才 true
    // 这里强制覆盖为 true（除非调用方显式 include_unpublished=0/false）
    {
        auto it = req.params.find("include_unpublished");
        if (it != req.params.end() &&
            (it->second == "0" || it->second == "false" || it->second == "False")) {
            parsed.query.include_unpublished = false;  // 显式要"只看已发布"
        } else {
            parsed.query.include_unpublished = true;   // 默认看到草稿
        }
    }

    oj::domain::ProblemListResult result;
    try {
        result = service->list(parsed.query);
    } catch (const std::exception& e) {
        spdlog::error("GET /api/admin/problems DB error: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }
    spdlog::info("GET /api/admin/problems admin={} page={} size={} total={} returned={}",
                 user_id, result.page, result.page_size,
                 result.total, result.items.size());
    write_ok(res, list_result_to_json(result));
}

// ---------------------------------------------------------------------------
//  GET /api/admin/problems/{id}/edit-data
//  返回含全部 testcases 的详情，给后台编辑表单用
// ---------------------------------------------------------------------------
void handle_edit_data(const std::shared_ptr<oj::domain::IProblemService>& service,
                      const std::shared_ptr<JwtService>& jwt,
                      const std::function<bool()>& is_db_ready,
                      const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    auto id_opt = parse_id_param(req);
    if (!id_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest, "id must be a positive integer");
        return;
    }
    const std::int64_t id = *id_opt;

    std::optional<oj::domain::AdminProblemDetail> detail;
    try {
        detail = service->get_admin_detail(id);
    } catch (const std::exception& e) {
        spdlog::error("GET /api/admin/problems/{}/edit-data DB error: {}", id, e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }
    if (!detail.has_value()) {
        write_error(res, ErrorCode::NotFound, "problem not found");
        return;
    }
    spdlog::info("GET /api/admin/problems/{}/edit-data admin={} cases={} tags={}",
                 id, user_id, detail->testcases.size(), detail->tags.size());
    write_ok(res, admin_detail_to_json(*detail));
}

// ---------------------------------------------------------------------------
//  POST /api/admin/problems
//  body: ProblemWriteInput 全部字段
//  resp: {code:0, data: Problem JSON}
// ---------------------------------------------------------------------------
void handle_create(const std::shared_ptr<oj::domain::IProblemService>& service,
                   const std::shared_ptr<JwtService>& jwt,
                   const std::function<bool()>& is_db_ready,
                   const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    // 1) 解析 body
    json body;
    try {
        if (req.body.empty()) {
            write_error(res, ErrorCode::BadRequest, "request body is empty");
            return;
        }
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest,
                    std::string{"invalid json: "} + e.what());
        return;
    }
    if (!body.is_object()) {
        write_error(res, ErrorCode::BadRequest, "request body must be a JSON object");
        return;
    }

    // 2) body → ProblemWriteInput
    ProblemWriteInput in;
    try {
        in = parse_problem_write_input(body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest, e.what());
        return;
    }

    // 3) 业务
    Problem created;
    try {
        created = service->create_problem(user_id, in);
    } catch (const AdminProblemError& e) {
        spdlog::info("create_problem rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_admin_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("create_problem internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    spdlog::info("POST /api/admin/problems admin={} new_id={} title='{}'",
                 user_id, created.id, created.title);
    write_ok(res, problem_to_json(created));
}

// ---------------------------------------------------------------------------
//  PUT /api/admin/problems/{id}
//  body: ProblemWriteInput 全部字段
// ---------------------------------------------------------------------------
void handle_update(const std::shared_ptr<oj::domain::IProblemService>& service,
                   const std::shared_ptr<JwtService>& jwt,
                   const std::function<bool()>& is_db_ready,
                   const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    auto id_opt = parse_id_param(req);
    if (!id_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest, "id must be a positive integer");
        return;
    }
    const std::int64_t id = *id_opt;

    // 1) 解析 body
    json body;
    try {
        if (req.body.empty()) {
            write_error(res, ErrorCode::BadRequest, "request body is empty");
            return;
        }
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest,
                    std::string{"invalid json: "} + e.what());
        return;
    }
    if (!body.is_object()) {
        write_error(res, ErrorCode::BadRequest, "request body must be a JSON object");
        return;
    }

    // 2) body → ProblemWriteInput
    ProblemWriteInput in;
    try {
        in = parse_problem_write_input(body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest, e.what());
        return;
    }

    // 3) 业务
    try {
        service->update_problem(id, in);
    } catch (const AdminProblemError& e) {
        spdlog::info("update_problem rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_admin_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("update_problem internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 4) 拉最新数据返回
    std::optional<oj::domain::AdminProblemDetail> detail;
    try {
        detail = service->get_admin_detail(id);
    } catch (const std::exception& e) {
        spdlog::warn("update_problem post-fetch: {}", e.what());
    }
    if (!detail.has_value()) {
        // 不太可能（update 成功却查不到）；返回最小 envelope
        write_ok(res, json{{"id", id}});
        return;
    }
    spdlog::info("PUT /api/admin/problems/{} admin={} cases={} tags={}",
                 id, user_id, detail->testcases.size(), detail->tags.size());
    write_ok(res, admin_detail_to_json(*detail));
}

// ---------------------------------------------------------------------------
//  DELETE /api/admin/problems/{id} —— 软删（is_published=0）
// ---------------------------------------------------------------------------
void handle_delete(const std::shared_ptr<oj::domain::IProblemService>& service,
                   const std::shared_ptr<JwtService>& jwt,
                   const std::function<bool()>& is_db_ready,
                   const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    auto id_opt = parse_id_param(req);
    if (!id_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest, "id must be a positive integer");
        return;
    }
    const std::int64_t id = *id_opt;

    try {
        service->delete_problem(id);
    } catch (const AdminProblemError& e) {
        spdlog::info("delete_problem rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_admin_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("delete_problem internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }
    spdlog::info("DELETE /api/admin/problems/{} admin={}", id, user_id);
    write_ok(res, nullptr);
}

// ---------------------------------------------------------------------------
//  PATCH /api/admin/problems/{id}/publish
//  body: {"is_published": true/false}
//  resp: {code:0, data: Problem JSON（含最新 is_published）}
// ---------------------------------------------------------------------------
void handle_publish(const std::shared_ptr<oj::domain::IProblemService>& service,
                    const std::shared_ptr<JwtService>& jwt,
                    const std::function<bool()>& is_db_ready,
                    const httplib::Request& req, httplib::Response& res) {
    if (!check_db(is_db_ready, res)) return;

    std::int64_t user_id = 0;
    if (!require_admin(req, jwt, user_id, res)) return;

    auto id_opt = parse_id_param(req);
    if (!id_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest, "id must be a positive integer");
        return;
    }
    const std::int64_t id = *id_opt;

    // 1) 解析 body
    json body;
    try {
        if (req.body.empty()) {
            write_error(res, ErrorCode::BadRequest, "request body is empty");
            return;
        }
        body = json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest,
                    std::string{"invalid json: "} + e.what());
        return;
    }
    if (!body.is_object()) {
        write_error(res, ErrorCode::BadRequest, "request body must be a JSON object");
        return;
    }
    bool want_published = false;
    {
        auto it = body.find("is_published");
        if (it == body.end() || !it->is_boolean()) {
            write_error(res, ErrorCode::BadRequest,
                        "is_published (boolean) is required");
            return;
        }
        want_published = it->get<bool>();
    }

    // 2) 业务
    try {
        service->set_published(id, want_published);
    } catch (const AdminProblemError& e) {
        spdlog::info("set_publish rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_admin_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("set_publish internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 3) 拉最新数据返回
    std::optional<Problem> p;
    try {
        // service.list 不便复用；用 IProblemService::get_detail(include_unpublished=true)
        // 即可拉到本题（含未发布）
        auto d = service->get_detail(id, /*include_unpublished=*/true);
        if (d.has_value()) p = d->problem;
    } catch (const std::exception& e) {
        spdlog::warn("set_publish post-fetch: {}", e.what());
    }
    if (!p.has_value()) {
        write_ok(res, json{{"id", id}, {"is_published", want_published}});
        return;
    }
    spdlog::info("PATCH /api/admin/problems/{}/publish admin={} -> {}",
                 id, user_id, want_published);
    write_ok(res, problem_to_json(*p));
}

}  // namespace

void register_admin_problem_routes(
    HttpServer& server,
    std::shared_ptr<oj::domain::IProblemService>  service,
    std::shared_ptr<oj::infra::JwtService>        jwt,
    std::function<bool()>                         is_db_ready)
{
    auto sp_svc   = std::move(service);
    auto sp_jwt   = std::move(jwt);
    auto sp_ready = std::move(is_db_ready);

    server.get("/api/admin/problems",
               [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                          httplib::Response& res) {
        handle_list(sp_svc, sp_jwt, sp_ready, req, res);
    });

    server.post("/api/admin/problems",
                [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                           httplib::Response& res) {
        handle_create(sp_svc, sp_jwt, sp_ready, req, res);
    });

    server.get("/api/admin/problems/:id/edit-data",
               [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                          httplib::Response& res) {
        handle_edit_data(sp_svc, sp_jwt, sp_ready, req, res);
    });

    server.put("/api/admin/problems/:id",
               [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                          httplib::Response& res) {
        handle_update(sp_svc, sp_jwt, sp_ready, req, res);
    });

    server.del("/api/admin/problems/:id",
               [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                          httplib::Response& res) {
        handle_delete(sp_svc, sp_jwt, sp_ready, req, res);
    });

    server.patch("/api/admin/problems/:id/publish",
                 [sp_svc, sp_jwt, sp_ready](const httplib::Request& req,
                                            httplib::Response& res) {
        handle_publish(sp_svc, sp_jwt, sp_ready, req, res);
    });
}

}  // namespace oj::http::handlers
