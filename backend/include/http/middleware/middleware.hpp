#pragma once

// =============================================================================
//  oj::http::middleware — Http 中间件层 (SPEC §3.2.2)
//
//  本目录承载 HTTP 层可复用的"横切关注点"组件:
//    - access_log  : 每条 HTTP 请求记录 access log (方法/路径/状态/耗时/user)
//    - error        : 统一错误响应 (JSON 信封) + 全局异常兜底
//    - security     : 安全响应头 (CSP / X-Content-Type-Options / X-Frame-Options)
//
//  设计要点:
//    1. 与 cpp-httplib 解耦 —— 中间件以"hook 注入"的形式挂入 HttpServer
//       (set_logger / set_exception_handler / set_error_handler),不依赖
//       cpp-httplib 的私有细节。
//    2. 与 Domain / Infra 解耦 —— 不持有任何 service / repo 指针,只通过
//       req.get_header_value("Authorization") 解析 JWT,失败则 user_id=0。
//    3. 可独立单测 —— 单元测试直接构造 Request/Response 调用中间件函数。
//
//  Phase 7.2: 统一错误中间件 (ErrorMiddleware) 实现在 error.hpp 里。
//  本文件 include 它,handler 只需 #include "http/middleware/middleware.hpp"
//  即可同时拿到 access_log / security / error 三件套。
// =============================================================================

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "http/middleware/error.hpp"  // 统一错误中间件 (HttpError / wrap_handler / ...)

namespace oj::http {
class HttpServer;  // forward declaration for middleware API
}

namespace oj::http::middleware {

// ---- access log ------------------------------------------------------------

// SPEC §2.6 可观测: "所有 HTTP 请求记录 access log (方法/路径/状态/耗时/user_id)"
//
// install_access_log: 在 HttpServer 上挂 set_logger hook,把每条请求
//  - method / path / status / latency_ms / remote_addr / user_id
// 写一行 spdlog info (latency_ms < warn_threshold 时) 或 warn。
//
// 阈值参数: warn_threshold_ms —— 超过该耗时打 warn;否则 info;0 表示全 warn。
void install_access_log(oj::http::HttpServer& server, int warn_threshold_ms = 1000);

// 解析 Authorization Header 中的 Bearer JWT,只截取 sub claim(数字 user_id);
// 失败或缺失 → 返回 0;成功 → 返回 user_id。
//
// 设计为 free function 而非方法,便于单元测试直接构造字符串喂入。
std::int64_t extract_user_id_from_bearer(const std::string& authz_header);

// ---- error / exception -----------------------------------------------------

// install_unified_error_handlers: 一次性挂好 set_exception_handler +
// set_error_handler。HttpServer::listen() 内部已经调过一份基线版本,
// 此函数给外部(测试 / 嵌入式场景)调用以补齐同样的统一错误响应。
//
// 与 HttpServer::install_exception_middleware 的区别:
//   - HttpServer::install_exception_middleware 是基线实现 (cp-httplib 必须的兜底)
//   - 这里允许替换错误消息 / 日志级别 / 状态码细节,但行为完全一致
//
// 该函数现在还负责确保 wrap_handler / HttpError 的所有符号可用 ——
// 实际 wrap 是 per-route opt-in(新 handler 显式包一层),不影响既有 handler。
void install_unified_error_handlers(oj::http::HttpServer& server);

// Phase 7.2 统一错误中间件的高阶 API 全部在 http/middleware/error.hpp 里:
//   - HttpError           业务层主动抛的"协议级"异常
//   - wrap_handler(h)     包一层 cpp-httplib handler,统一 catch HttpError / std::exception
//   - check_db_ready      "DB 不可用"1008 envelope 的"判断+写"二合一
//   - parse_path_id       解析 :id 路径参数 → optional<int64>
//   - parse_query_int     解析 ?key=123 query 参数 → optional<int64>
//   - require_string_field 从 json body 提取 string 字段,缺/类型错抛 1001
//
// handler 推荐新写法:
//     server.post("/api/x", mw::wrap_handler([&](const auto& req, auto& res) {
//         if (some_validation_failed)
//             throw mw::HttpError::bad_request("xxx");
//         // ... 业务
//         write_ok(res, data);
//     }));

// ---- security headers ------------------------------------------------------

// 加固响应头 —— SPEC §9.3 S-1:
//   - X-Content-Type-Options: nosniff
//   - X-Frame-Options:        DENY
//   - Referrer-Policy:        no-referrer
//   - Content-Security-Policy: 适度严格(允许 jsdelivr CDN 加载 Monaco 与 markdown-it)
//
// 注意: nginx 反代层也会加这些头;这里给直连 8080 提供兜底,避免漏配 nginx 时裸奔。
//
// CSP 策略(SPEC §3.3.4: Monaco + markdown-it 从 jsdelivr CDN 加载):
//   - default-src 'self'
//   - script-src  'self' 'unsafe-inline' 'unsafe-eval' https://cdn.jsdelivr.net
//                 (Monaco 启动时动态插入 <script>,需要 unsafe-inline;
//                  Worker 加载需要 unsafe-eval,无法避免)
//   - style-src   'self' 'unsafe-inline' https://cdn.jsdelivr.net
//   - img-src     'self' data: https:;
//   - font-src    'self' data: https://cdn.jsdelivr.net;
//   - connect-src 'self';
//   - frame-ancestors 'none';  (等同 X-Frame-Options: DENY)
//
// install_security_headers: 挂 set_post_routing_handler,对所有非 /api/* 的
// 响应(API 响应 Content-Type 已经是 JSON,意义不大)与 /api/* 响应都加头。
void install_security_headers(oj::http::HttpServer& server);

// ---- request helpers (统一请求体解析,handler 复用) --------------------------

// parse_json_body: 统一"POST/PUT 请求体解析为 JSON object"的样板。
// 返回 std::nullopt 表示已向 res 写入错误信封(1001 BadRequest),handler
// 应立即 return;返回有效 json 表示解析成功,handler 继续。
//
// 错误码:
//   - body 为空       → 1001 + "request body is empty"
//   - 解析抛异常      → 1001 + "invalid json: ..."
//   - 顶层不是 object → 1001 + "request body must be a JSON object"
//
// 此前 4 个 handler (auth / submission / admin_problem) 各自复制了这段
// 30+ 行样板;统一后等价行为 + 减少 ~120 行重复。
std::optional<nlohmann::json> parse_json_body(const httplib::Request& req,
                                              httplib::Response&       res);

// db_unavailable_response: 写"数据库不可用"信封 (1008 SystemError),
// 给所有 handler 共享一个一致的兜底。
void db_unavailable_response(httplib::Response& res);

}  // namespace oj::http::middleware