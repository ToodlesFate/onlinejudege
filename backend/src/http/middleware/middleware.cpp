#include "http/middleware/middleware.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/error_code.hpp"
#include "http/HttpServer.hpp"  // HttpServer 完整定义 (install_* 调用)

namespace oj::http::middleware {

// ----------------------------------------------------------------------------
//  access log
// ----------------------------------------------------------------------------

std::int64_t extract_user_id_from_bearer(const std::string& authz_header) {
    // 期望形如: "Bearer eyJhbGciOi..." (大小写不敏感)
    // 这里只解析前缀 + 用 base64url 解码 payload,再掏 "sub";不验签 —
    // 真正的鉴权由 AuthMiddleware 在 handler 入口前完成,这里只是日志维度
    // 的"打点 user_id"。即使 JWT 是伪造的,后续也会被鉴权中间件拦截,
    // 不会污染业务路径。
    if (authz_header.size() < 8) return 0;
    // 期望 "Bearer " (大小写不敏感,SPEC §2.1 access token 走 Authorization: Bearer ...)
    constexpr std::string_view kLowerPrefix = "bearer ";
    bool ok = true;
    for (std::size_t i = 0; i < kLowerPrefix.size(); ++i) {
        char c = authz_header[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != kLowerPrefix[i]) { ok = false; break; }
    }
    if (!ok) return 0;

    const std::size_t dot1 = authz_header.find('.', kLowerPrefix.size());
    if (dot1 == std::string::npos) return 0;
    const std::size_t dot2 = authz_header.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return 0;

    // payload = authz_header[dot1+1 .. dot2-1] (base64url, no padding)
    std::string payload = authz_header.substr(dot1 + 1, dot2 - dot1 - 1);

    // base64url decode —— 只为掏 "sub";不验签 / 不查 exp
    // (cpp-httplib 内部 detail::base64_decode 不导出,自己写一份精简版)
    auto decode_b64url = [](std::string_view in) -> std::string {
        auto val = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '-') return 62;
            if (c == '_') return 63;
            return -1;
        };
        std::string out;
        out.reserve(in.size() * 3 / 4);
        int buf = 0, bits = 0;
        for (char c : in) {
            int v = val(c);
            if (v < 0) continue;  // 容错:跳过非法字符
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buf >> bits) & 0xFF));
            }
        }
        return out;
    };

    std::string decoded = decode_b64url(payload);

    // 粗扫 user id 字段:本项目 JwtService 用 "uid" 申领;为兼容未来 RFC 7519
    // 标准,也认 "sub"。先找 "uid",再找 "sub"。
    auto find_user_id = [&decoded](const char* key) -> std::int64_t {
        std::string needle = std::string{"\""} + key + "\"";
        const std::size_t pos = decoded.find(needle);
        if (pos == std::string::npos) return 0;
        const std::size_t colon = decoded.find(':', pos);
        if (colon == std::string::npos) return 0;

        std::int64_t v = 0;
        bool any = false;
        for (std::size_t i = colon + 1; i < decoded.size(); ++i) {
            char c = decoded[i];
            if (c >= '0' && c <= '9') {
                v = v * 10 + (c - '0');
                any = true;
            } else if (c == ',' || c == '}' || c == ' ' || c == '\n') {
                break;
            } else if (c == '"') {
                // string 类型 sub —— 不应出现但容错,直接放弃
                return 0;
            }
            // 其它字符(空格 / 制表)跳过
        }
        return any ? v : 0;
    };

    if (auto uid = find_user_id("uid"); uid > 0) return uid;
    if (auto sub = find_user_id("sub"); sub > 0) return sub;
    return 0;
}

void install_access_log(HttpServer& server, int warn_threshold_ms) {
    // 阈值 ≤ 0 → 一律 warn;否则 info,超阈值升 warn
    const int threshold = warn_threshold_ms > 0 ? warn_threshold_ms : 0;

    // cpp-httplib 的 Request 字段在 logger 回调里是 const,且没有 start_time_,
    // 所以我们用 thread_local 在 pre_routing 阶段打点,在 logger 阶段读出。
    // thread pool 内的 worker 线程对同一时刻只有一个请求在跑,不会撞车。
    static thread_local std::chrono::steady_clock::time_point t_req_start{};

    server.install_pre_routing([](const httplib::Request& /*req*/,
                                  httplib::Response& /*res*/) {
        t_req_start = std::chrono::steady_clock::now();
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.install_logger([threshold](const httplib::Request& req,
                                      const httplib::Response& res) {
        const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - t_req_start)
                                    .count();

        const std::int64_t user_id =
            extract_user_id_from_bearer(req.get_header_value("Authorization"));

        const auto level = (threshold > 0 &&
                            static_cast<long long>(latency_ms) >= threshold)
                               ? spdlog::level::warn
                               : spdlog::level::info;

        // 格式: METHOD PATH -> STATUS (LATENCYms, user=UID) [REMOTE]
        // 单行,便于 awk / grep 后续处理
        spdlog::log(level,
                    "{} {} -> {} ({}ms, user={}) [{}]",
                    req.method, req.path, res.status,
                    static_cast<long long>(latency_ms),
                    user_id, req.remote_addr);
    });
}

void install_unified_error_handlers(HttpServer& server) {
    // 当前 HttpServer 基线已经统一处理了 4xx/5xx 与 unhandled exception,
    // 本函数作为对外的稳定 API 给测试 / 嵌入式复用,内部直接调 HttpServer 基线。
    //
    // 业务层新增的 HttpError / wrap_handler 是 per-route opt-in(在
    // register_auth_routes 等地方显式套 wrap_handler),不需要在 server 级别
    // 额外挂 hook —— wrap_handler 内部会 catch HttpError 后写 envelope。
    //
    // 这里 install_exception_middleware 的作用是"双保险":
    //   - 任何未被 wrap_handler 包过的 handler 抛异常 → 1007 envelope
    //   - 已经被 wrap_handler 包过的 handler 抛 HttpError → wrap_handler 内部
    //     已处理;若意外漏出 std::exception,仍由 set_exception_handler 兜底
    server.install_exception_middleware();
}

// ----------------------------------------------------------------------------
//  security headers
// ----------------------------------------------------------------------------

namespace {

constexpr const char* kCspHeader =
    "default-src 'self'; "
    "script-src 'self' 'unsafe-inline' 'unsafe-eval' https://cdn.jsdelivr.net; "
    "style-src 'self' 'unsafe-inline' https://cdn.jsdelivr.net; "
    "img-src 'self' data: https:; "
    "font-src 'self' data: https://cdn.jsdelivr.net; "
    "connect-src 'self'; "
    "frame-ancestors 'none'; "
    "base-uri 'self'; "
    "form-action 'self'";

void add_security_headers(httplib::Response& res) {
    // S-1: 安全响应头 (SPEC §9.3)
    // - X-Content-Type-Options: nosniff   禁止 MIME 嗅探
    // - X-Frame-Options: DENY             禁止 iframe 嵌入 (防 clickjacking)
    // - Referrer-Policy: no-referrer      不向第三方泄露来源 URL
    // - Content-Security-Policy: 见上方宏
    if (res.get_header_value("X-Content-Type-Options").empty()) {
        res.set_header("X-Content-Type-Options", "nosniff");
    }
    if (res.get_header_value("X-Frame-Options").empty()) {
        res.set_header("X-Frame-Options", "DENY");
    }
    if (res.get_header_value("Referrer-Policy").empty()) {
        res.set_header("Referrer-Policy", "no-referrer");
    }
    if (res.get_header_value("Content-Security-Policy").empty()) {
        res.set_header("Content-Security-Policy", kCspHeader);
    }
}

}  // namespace

void install_security_headers(HttpServer& server) {
    server.install_post_routing([](const httplib::Request&, httplib::Response& res) {
        add_security_headers(res);
    });
}

// ----------------------------------------------------------------------------
//  request helpers (统一请求体解析)
// ----------------------------------------------------------------------------

std::optional<nlohmann::json> parse_json_body(const httplib::Request& req,
                                              httplib::Response&       res) {
    using oj::common::ErrorCode;

    if (req.body.empty()) {
        write_error(res, ErrorCode::BadRequest, "request body is empty");
        return std::nullopt;
    }
    try {
        auto body = nlohmann::json::parse(req.body);
        if (!body.is_object()) {
            write_error(res, ErrorCode::BadRequest,
                        "request body must be a JSON object");
            return std::nullopt;
        }
        return body;
    } catch (const std::exception& e) {
        write_error(res, ErrorCode::BadRequest,
                    std::string{"invalid json: "} + e.what());
        return std::nullopt;
    }
}

void db_unavailable_response(httplib::Response& res) {
    using oj::common::ErrorCode;
    write_error(res, ErrorCode::SystemError, "database not available");
}

}  // namespace oj::http::middleware