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

// 把 RefreshError 翻译为 ErrorCode
oj::common::ErrorCode map_refresh_error(oj::domain::RefreshErrorKind k) {
    using oj::domain::RefreshErrorKind;
    switch (k) {
        case RefreshErrorKind::BadRequest:   return oj::common::ErrorCode::BadRequest;
        case RefreshErrorKind::Unauthorized: return oj::common::ErrorCode::Unauthorized;
        case RefreshErrorKind::Internal:     return oj::common::ErrorCode::Internal;
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

// 拼"清空 refresh_token"的 Set-Cookie —— 用于 /api/auth/logout 与 refresh
// 失败回滚（让浏览器立刻清掉坏 refresh）。Max-Age=0 + 空 value 是 RFC 6265
// 推荐的删除语义。
std::string build_clear_refresh_cookie() {
    return "refresh_token=; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=0";
}

// 从 Cookie 头里提取指定 name 的原始 value。
//   - 输入不区分大小写、允许 Cookie 头是空 / 多对 / 前后有空白
//   - 不做 URL-decode（JWT 字符集是 base64url，本身不含需要 encode 的字符）
//   - 找不到 → std::nullopt
// 专给 refresh 用 —— 故意保持简单，避免引入完整的 Cookie 解析库。
std::optional<std::string> get_cookie_value(const std::string& cookie_header,
                                            std::string_view name) {
    if (cookie_header.empty() || name.empty()) return std::nullopt;

    std::size_t pos = 0;
    while (pos < cookie_header.size()) {
        // 跳过分号 / 空白
        while (pos < cookie_header.size() &&
               (cookie_header[pos] == ';' || cookie_header[pos] == ' ' ||
                cookie_header[pos] == '\t')) {
            ++pos;
        }
        if (pos >= cookie_header.size()) break;

        // 读 "key=value" 对，到 ';' 或字符串末尾
        const std::size_t pair_start = pos;
        std::size_t       eq_pos     = std::string::npos;
        while (pos < cookie_header.size() && cookie_header[pos] != ';') {
            if (eq_pos == std::string::npos && cookie_header[pos] == '=') {
                eq_pos = pos;
            }
            ++pos;
        }
        const std::size_t pair_end = pos;  // [pair_start, pair_end) 是不含尾分号的一段

        if (eq_pos != std::string::npos) {
            const std::string_view key(cookie_header.data() + pair_start,
                                       eq_pos - pair_start);
            if (key == name) {
                // value 段 = [eq_pos+1, pair_end)，首尾空白 trim
                std::size_t v_begin = eq_pos + 1;
                std::size_t v_end   = pair_end;
                while (v_begin < v_end &&
                       (cookie_header[v_begin] == ' ' || cookie_header[v_begin] == '\t')) {
                    ++v_begin;
                }
                while (v_end > v_begin &&
                       (cookie_header[v_end - 1] == ' ' || cookie_header[v_end - 1] == '\t')) {
                    --v_end;
                }
                return std::string(cookie_header.data() + v_begin, v_end - v_begin);
            }
        }
        // 跳过这一对后面的分号
        if (pos < cookie_header.size() && cookie_header[pos] == ';') ++pos;
    }
    return std::nullopt;
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

// POST /api/auth/refresh 的 handler 闭包 —— SPEC §2.1 + §5.2.1
//   鉴权 : 从 Cookie 头里读 refresh_token
//   resp : {"code":0,"message":"ok","data":{"access_token"}}
//   header: Set-Cookie: refresh_token=<轮换后新值>; ...; Max-Age=...
void handle_refresh(const std::shared_ptr<oj::domain::AuthService>& auth,
                    const std::function<bool()>& is_db_ready,
                    const httplib::Request& req, httplib::Response& res) {
    using oj::common::ErrorCode;

    // 0) DB 可用性检查 —— MySQL 不可达时返回 503（SPEC §2.6 可用性）
    if (is_db_ready && !is_db_ready()) {
        write_error(res, ErrorCode::SystemError, "database not available");
        return;
    }

    // 1) 从 Cookie 头里取 refresh_token
    const std::string cookie_header = req.get_header_value("Cookie");
    auto token = get_cookie_value(cookie_header, "refresh_token");
    if (!token.has_value() || token->empty()) {
        // 缺 cookie —— SPEC 视作"请求格式不对"（1001 BadRequest）。
        // 注意：业务上更"对"的做法是把"cookie 缺失"也归到 1002 统一对外，
        // 但因为 1001 与 1002 的前端处理（前者直接重登，后者先尝试 refresh
        // 一次）不同，这里给前端一个明确信号："你连 refresh cookie 都没有，
        // 直接去登录页"，避免无谓的 401 噪声。
        write_error(res, ErrorCode::BadRequest, "missing refresh_token cookie");
        return;
    }

    // 2) 调 AuthService
    oj::domain::RefreshResult result;
    try {
        result = auth->refresh_access(*token);
    } catch (const oj::domain::RefreshError& e) {
        spdlog::info("refresh rejected: {} (kind={})",
                     e.what(), static_cast<int>(e.kind()));
        // 签名错 / 过期 / 用户已删除等 —— 防御性把客户端的旧 refresh 也清掉，
        // 避免浏览器里残留坏 token 后再次被使用（虽然 401 也不会让业务出错，
        // 但提前清掉更干净）
        res.set_header("Set-Cookie", build_clear_refresh_cookie());
        write_error(res, map_refresh_error(e.kind()), e.what());
        return;
    } catch (const std::exception& e) {
        spdlog::error("refresh internal error: {}", e.what());
        res.set_header("Set-Cookie", build_clear_refresh_cookie());
        write_error(res, ErrorCode::Internal, "internal server error");
        return;
    }

    // 3) 写轮换后的新 refresh_token cookie
    res.set_header("Set-Cookie",
                   build_refresh_cookie(result.refresh_token, auth->refresh_ttl_sec()));

    // 4) 返回 SPEC §5.2.1 的 data 形状（仅 access_token，refresh 走 cookie）
    nlohmann::json data = {
        {"access_token", result.access_token},
    };
    write_ok(res, std::move(data));
    spdlog::info("refresh ok: user_id={} is_admin={}", result.user_id, result.is_admin);
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
    server.post("/api/auth/refresh", [sp_auth, sp_ready](const httplib::Request& req, httplib::Response& res) {
        handle_refresh(sp_auth, sp_ready, req, res);
    });
}

}  // namespace oj::http::handlers
