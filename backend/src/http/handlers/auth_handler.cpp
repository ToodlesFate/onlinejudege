#include "http/handlers/auth_handler.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "common/error_code.hpp"
#include "http/HttpServer.hpp"  // HttpServer 完整定义（register_auth_routes 用到）

namespace oj::http::handlers {

namespace {

// 把 RegisterError 翻译为 ErrorCode；未知一律按 Internal 处理
oj::common::ErrorCode map_register_error(oj::domain::RegisterErrorKind k) {
    using oj::domain::RegisterErrorKind;
    switch (k) {
        case RegisterErrorKind::BadRequest: return oj::common::ErrorCode::BadRequest;
        case RegisterErrorKind::Conflict:   return oj::common::ErrorCode::Conflict;
        case RegisterErrorKind::Internal:   return oj::common::ErrorCode::Internal;
    }
    return oj::common::ErrorCode::Internal;
}

// 把 LoginError 翻译为 ErrorCode
oj::common::ErrorCode map_login_error(oj::domain::LoginErrorKind k) {
    using oj::domain::LoginErrorKind;
    switch (k) {
        case LoginErrorKind::BadRequest:   return oj::common::ErrorCode::BadRequest;
        case LoginErrorKind::Unauthorized: return oj::common::ErrorCode::Unauthorized;
        case LoginErrorKind::Internal:     return oj::common::ErrorCode::Internal;
    }
    return oj::common::ErrorCode::Internal;
}

// 拼 refresh_token Cookie —— SPEC §5.1
//     Set-Cookie: refresh_token=...; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=...
std::string build_refresh_cookie(const std::string& token, int max_age_sec) {
    std::string safe;
    safe.reserve(token.size());
    for (char c : token) {
        if (c == '\r' || c == '\n' || c == ';') continue;
        safe.push_back(c);
    }
    return "refresh_token=" + safe +
           "; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=" +
           std::to_string(max_age_sec > 0 ? max_age_sec : 0);
}

// POST /api/auth/register 的 handler 闭包
void handle_register(const std::shared_ptr<oj::domain::AuthService>& auth,
                     const std::function<bool()>& is_db_ready,
                     const httplib::Request& req, httplib::Response& res) {
    using oj::common::ErrorCode;

    // 0) DB 可用性检查 —— MySQL 不可达时返回 503（SPEC §2.6 可用性）
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 1) 解析 body
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

    // 2) 提取字段
    auto get_string = [&](const char* key) -> std::string {
        auto it = body.find(key);
        if (it == body.end() || !it->is_string()) return {};
        return it->get<std::string>();
    };
    const std::string username = get_string("username");
    const std::string email    = get_string("email");
    const std::string password = get_string("password");
    if (username.empty() || email.empty() || password.empty()) {
        write_error(res, ErrorCode::BadRequest,
                    "username, email and password are required");
        return;
    }

    // 3) 调 AuthService
    oj::domain::RegisterResult result;
    try {
        result = auth->register_user(username, email, password);
    } catch (const oj::domain::RegisterError& e) {
        spdlog::info("register rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_register_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("register internal error: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 4) 写 refresh_token cookie
    res.set_header("Set-Cookie",
                   build_refresh_cookie(result.refresh_token, auth->refresh_ttl_sec()));

    // 5) 返回 SPEC §5.2.1 的 data 形状
    nlohmann::json data = {
        {"user_id",      result.user_id},
        {"access_token", result.access_token},
        {"is_admin",     result.is_admin},
    };
    write_ok(res, std::move(data));
    spdlog::info("register ok: user_id={} username='{}' is_admin={}",
                 result.user_id, username, result.is_admin);
}

// POST /api/auth/login 的 handler 闭包 —— SPEC §5.2.1
//   body  : {"username": "...", "password": "..."}
//   resp  : {"code":0,"message":"ok","data":{"user_id","access_token","is_admin"}}
//   header: Set-Cookie: refresh_token=...; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=...
void handle_login(const std::shared_ptr<oj::domain::AuthService>& auth,
                  const std::function<bool()>& is_db_ready,
                  const httplib::Request& req, httplib::Response& res) {
    using oj::common::ErrorCode;

    // 0) DB 可用性检查 —— MySQL 不可达时返回 503（SPEC §2.6 可用性）
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 1) 解析 body
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

    // 2) 提取字段
    auto get_string = [&](const char* key) -> std::string {
        auto it = body.find(key);
        if (it == body.end() || !it->is_string()) return {};
        return it->get<std::string>();
    };
    const std::string username = get_string("username");
    const std::string password = get_string("password");
    if (username.empty() || password.empty()) {
        write_error(res, ErrorCode::BadRequest,
                    "username and password are required");
        return;
    }

    // 3) 调 AuthService
    oj::domain::LoginResult result;
    try {
        result = auth->login_user(username, password);
    } catch (const oj::domain::LoginError& e) {
        // 日志侧记录 kind（便于排查），对外按 Unauthorized 一律 401
        // —— AuthService 已经统一过 message 为 "invalid username or password"
        spdlog::info("login rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        write_error(res, map_login_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("login internal error: {}", e.what());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 4) 写 refresh_token cookie（与 register 完全一致的 Set-Cookie 形状）
    res.set_header("Set-Cookie",
                   build_refresh_cookie(result.refresh_token, auth->refresh_ttl_sec()));

    // 5) 返回 SPEC §5.2.1 的 data 形状
    nlohmann::json data = {
        {"user_id",      result.user_id},
        {"access_token", result.access_token},
        {"is_admin",     result.is_admin},
    };
    write_ok(res, std::move(data));
    spdlog::info("login ok: user_id={} username='{}' is_admin={}",
                 result.user_id, username, result.is_admin);
}

}  // namespace

void register_auth_routes(HttpServer& server,
                          std::shared_ptr<oj::domain::AuthService> auth,
                          std::function<bool()> is_db_ready) {
    auto sp_auth = std::move(auth);
    auto sp_ready = std::move(is_db_ready);
    server.post("/api/auth/register", [sp_auth, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_register(sp_auth, sp_ready, req, res);
    });
    server.post("/api/auth/login", [sp_auth, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_login(sp_auth, sp_ready, req, res);
    });
}

}  // namespace oj::http::handlers
