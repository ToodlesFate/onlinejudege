// =============================================================================
//  submission_handler.cpp —— POST/GET /api/submissions 实现
//  SPEC §2.3 / §5.2.3
// =============================================================================

#include "http/handlers/submission_handler.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "common/error_code.hpp"
#include "domain/problem_types.hpp"
#include "domain/submission_service.hpp"
#include "domain/submission_types.hpp"
#include "http/HttpServer.hpp"
#include "infra/jwt_service.hpp"

namespace oj::http::handlers {

namespace {

using oj::common::ErrorCode;
using nlohmann::json;
using oj::domain::CreateSubmissionError;
using oj::domain::CreateSubmissionErrorKind;
using oj::domain::GetSubmissionError;
using oj::domain::GetSubmissionErrorKind;
using oj::domain::Language;
using oj::domain::SubmissionCase;
using oj::domain::SubmissionDetail;
using oj::domain::SubmissionResult;
using oj::domain::SubmissionStatus;
using oj::infra::InvalidToken;
using oj::infra::JwtService;

// ---------------------------------------------------------------------------
//  把 CreateSubmissionErrorKind 翻译成 ErrorCode
// ---------------------------------------------------------------------------
ErrorCode map_create_error(CreateSubmissionErrorKind k) noexcept {
    switch (k) {
        case CreateSubmissionErrorKind::BadRequest: return ErrorCode::BadRequest;
        case CreateSubmissionErrorKind::NotFound:   return ErrorCode::NotFound;
        case CreateSubmissionErrorKind::TooLarge:   return ErrorCode::TooLarge;
        case CreateSubmissionErrorKind::Internal:   return ErrorCode::Internal;
    }
    return ErrorCode::Internal;
}

ErrorCode map_get_error(GetSubmissionErrorKind k) noexcept {
    switch (k) {
        case GetSubmissionErrorKind::Forbidden: return ErrorCode::Forbidden;
        case GetSubmissionErrorKind::Internal:  return ErrorCode::Internal;
    }
    return ErrorCode::Internal;
}

// ---------------------------------------------------------------------------
//  SubmissionCase → JSON —— 字段顺序与 SPEC §5.3 示例一致
//  - is_sample=1：填 input / expected_output / user_output
//  - is_sample=0：三个字段都 null（隐藏点不能泄漏）
// ---------------------------------------------------------------------------
json case_to_json(const SubmissionCase& c) {
    json j = {
        {"case_index",      c.case_index},
        {"status",          oj::domain::to_string(c.status)},
        {"time_used_ms",    c.time_used_ms},
        {"memory_used_kb",  c.memory_used_kb},
        {"score",           c.score},
        {"is_sample",       c.is_sample},
    };
    if (c.is_sample) {
        j["user_output"]     = c.user_output;
        j["input"]           = c.input;
        j["expected_output"] = c.expected_output;
    } else {
        j["user_output"]     = nullptr;
        j["input"]           = nullptr;
        j["expected_output"] = nullptr;
    }
    return j;
}

// ---------------------------------------------------------------------------
//  SubmissionDetail → JSON —— SPEC §5.3 示例
//  字段顺序：id / problem_id / user_id / username / language / code
//            / status / result / total_score / time_used_ms / memory_used_kb
//            / compile_output / judge_message / created_at / finished_at / cases
//  result：未完成时为 null（SubmissionResult 还未设置）
// ---------------------------------------------------------------------------
json detail_to_json(const SubmissionDetail& d) {
    const auto& s = d.submission;
    json j = {
        {"id",             s.id},
        {"problem_id",     s.problem_id},
        {"user_id",        s.user_id},
        {"username",       d.username},
        {"language",       oj::domain::to_string(s.language)},
        {"code",           s.code},
        {"status",         oj::domain::to_string(s.status)},
        {"result",         s.result.has_value() ? json(std::string{oj::domain::to_string(*s.result)})
                                                 : json(nullptr)},
        {"total_score",    s.total_score},
        {"time_used_ms",   s.time_used_ms},
        {"memory_used_kb", s.memory_used_kb},
        {"compile_output", s.compile_output},
        {"judge_message",  s.judge_message},
        {"created_at",     s.created_at},
        {"finished_at",    s.finished_at},
    };
    json cases = json::array();
    for (const auto& c : d.cases) cases.push_back(case_to_json(c));
    j["cases"] = std::move(cases);
    return j;
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
//  从 JSON body 中安全取字符串字段
// ---------------------------------------------------------------------------
std::string get_json_string(const nlohmann::json& body, const char* key) {
    auto it = body.find(key);
    if (it == body.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

// ---------------------------------------------------------------------------
//  POST /api/submissions
//
//  body  : {"problem_id": <int>, "language": "cpp", "code": "..."}
//  resp  : {"code":0,"message":"ok","data":{"submission_id": <int>}}
//  auth  : Authorization: Bearer <access_token>（必须）
// ---------------------------------------------------------------------------
void handle_create(const std::shared_ptr<oj::domain::ISubmissionService>& service,
                   const std::shared_ptr<JwtService>& jwt,
                   const std::function<bool()>& is_db_ready,
                   const httplib::Request& req, httplib::Response& res) {
    // 0) DB readiness
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 1) 鉴权 —— POST 必须登录
    auto auth = parse_bearer_auth(req, jwt);
    if (!auth || !auth->authenticated) {
        write_error(res, ErrorCode::Unauthorized,
                    "missing or invalid access token");
        return;
    }

    // 2) 解析 body
    nlohmann::json body;
    try {
        if (req.body.empty()) {
            write_error(res, ErrorCode::BadRequest, "request body is empty");
            return;
        }
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest,
                    std::string{"invalid json: "} + e.what());
        return;
    }
    if (!body.is_object()) {
        write_error(res, ErrorCode::BadRequest, "request body must be a JSON object");
        return;
    }

    // 3) 字段提取 + 校验
    std::int64_t problem_id = 0;
    {
        auto it = body.find("problem_id");
        if (it == body.end() || !it->is_number_integer()) {
            write_error(res, ErrorCode::BadRequest,
                        "problem_id must be a positive integer");
            return;
        }
        const auto v = it->get<std::int64_t>();
        if (v <= 0) {
            write_error(res, ErrorCode::BadRequest,
                        "problem_id must be a positive integer");
            return;
        }
        problem_id = v;
    }

    const std::string language_str = get_json_string(body, "language");
    auto lang_opt = oj::domain::language_from_string(language_str);
    if (!lang_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest,
                    "language must be one of: c, cpp, java, python, go");
        return;
    }
    const Language language = *lang_opt;

    const std::string code = get_json_string(body, "code");
    if (code.empty()) {
        write_error(res, ErrorCode::BadRequest, "code is required");
        return;
    }

    // 4) 业务
    std::int64_t new_id = 0;
    try {
        new_id = service->create(auth->user_id, problem_id, language, code);
    } catch (const CreateSubmissionError& e) {
        spdlog::info("create submission rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_create_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("create submission internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 5) 成功
    spdlog::info("create submission ok: id={} user_id={} problem_id={} lang={} code_bytes={}",
                 new_id, auth->user_id, problem_id,
                 oj::domain::to_string(language), code.size());
    write_ok(res, json{{"submission_id", new_id}});
}

// ---------------------------------------------------------------------------
//  GET /api/submissions/{id}
//
//  resp  : 200 + envelope.data = submission detail
//        | 404 submission not found
//        | 403 forbidden
//        | 500 内部异常
//  auth  : 可选；登录后只有本人/admin 能看非 AC 提交
// ---------------------------------------------------------------------------
void handle_detail(const std::shared_ptr<oj::domain::ISubmissionService>& service,
                   const std::shared_ptr<JwtService>& jwt,
                   const std::function<bool()>& is_db_ready,
                   const httplib::Request& req, httplib::Response& res) {
    // 0) DB readiness
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 1) 解析 :id
    auto id_opt = parse_id_param(req);
    if (!id_opt.has_value()) {
        write_error(res, ErrorCode::BadRequest, "id must be a positive integer");
        return;
    }
    const std::int64_t id = *id_opt;

    // 2) 鉴权（可选）—— 匿名也允许访问 AC 提交
    auto auth = parse_bearer_auth(req, jwt);
    const std::int64_t requester_id = auth && auth->authenticated ? auth->user_id : 0;
    const bool         is_admin    = auth && auth->authenticated ? auth->is_admin : false;

    // 3) 业务
    std::optional<SubmissionDetail> detail;
    try {
        detail = service->get_detail(id, requester_id, is_admin);
    } catch (const GetSubmissionError& e) {
        spdlog::info("get submission rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_get_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("get submission internal err: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }
    if (!detail.has_value()) {
        write_error(res, ErrorCode::NotFound, "submission not found");
        return;
    }

    // 4) 成功
    spdlog::info("get submission ok: id={} requester_id={} admin={}",
                 id, requester_id, is_admin);
    write_ok(res, detail_to_json(*detail));
}

}  // namespace

// ---------------------------------------------------------------------------
//  parse_bearer_auth —— Authorization: Bearer <token> 解析
//  缺失 / 格式错 / 验签失败 → 返回 nullopt（调用方决定 401 还是匿名）
// ---------------------------------------------------------------------------
std::optional<AuthContext> parse_bearer_auth(
    const httplib::Request& req,
    const std::shared_ptr<JwtService>& jwt)
{
    if (!jwt) return std::nullopt;
    const std::string header = req.get_header_value("Authorization");
    if (header.empty()) return std::nullopt;

    static constexpr std::string_view kPrefix = "Bearer ";
    if (header.size() <= kPrefix.size() ||
        header.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    std::string_view token{header};
    token.remove_prefix(kPrefix.size());
    // trim 前后空白
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
        token.remove_prefix(1);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
        token.remove_suffix(1);
    }
    if (token.empty()) return std::nullopt;

    try {
        auto claims = jwt->verify(token, "access");
        AuthContext ctx;
        ctx.authenticated = true;
        ctx.user_id       = claims.user_id;
        ctx.is_admin      = claims.is_admin;
        return ctx;
    } catch (const InvalidToken&) {
        return std::nullopt;
    } catch (const std::exception& e) {
        spdlog::warn("parse_bearer_auth unexpected err: {}", e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
//  路由注册
// ---------------------------------------------------------------------------
void register_submission_routes(HttpServer& server,
                                 std::shared_ptr<oj::domain::ISubmissionService> service,
                                 std::shared_ptr<oj::infra::JwtService> jwt,
                                 std::function<bool()> is_db_ready) {
    auto sp_service = std::move(service);
    auto sp_jwt     = std::move(jwt);
    auto sp_ready   = std::move(is_db_ready);

    server.post("/api/submissions",
                [sp_service, sp_jwt, sp_ready](const httplib::Request& req,
                                                httplib::Response& res) {
        handle_create(sp_service, sp_jwt, sp_ready, req, res);
    });
    server.get("/api/submissions/:id",
               [sp_service, sp_jwt, sp_ready](const httplib::Request& req,
                                               httplib::Response& res) {
        handle_detail(sp_service, sp_jwt, sp_ready, req, res);
    });
}

}  // namespace oj::http::handlers
