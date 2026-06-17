#pragma once

// =============================================================================
//  oj::http::handlers::submission —— 提交相关 HTTP 入口
//  本阶段实现：
//    - POST /api/submissions        SPEC §5.2.3
//    - GET  /api/submissions/{id}   SPEC §5.2.3
//
//  注册方式：
//    register_submission_routes(server, service, jwt, is_db_ready)
//
//  鉴权：
//    POST  —— 必须带 Authorization: Bearer <access_token>（JwtService 校验）
//    GET   —— 鉴权可选：
//               * 匿名 / 其他用户：仅能看 result=AC
//               * 本人 / admin：可看全部
// =============================================================================

#include <functional>
#include <memory>
#include <optional>

#include "domain/submission_service.hpp"
#include "http/HttpServer.hpp"  // HttpServer 完整定义
#include "infra/jwt_service.hpp"

namespace oj::http::handlers {

// 当前请求的鉴权上下文（GET 接口里"未鉴权"也算合法状态）
struct AuthContext {
    bool         authenticated{false};
    std::int64_t user_id{0};
    bool         is_admin{false};
};

// 从 Authorization: Bearer <token> 里解析 JWT；缺失 / 格式错 / 验签失败 → 未鉴权
//   POST 路径：authenticated=false 时 handler 应返回 401
//   GET  路径：authenticated=false 时视为匿名访问，service 按可见性规则放行 AC
std::optional<AuthContext> parse_bearer_auth(
    const httplib::Request& req,
    const std::shared_ptr<oj::infra::JwtService>& jwt);

// 注册 POST /api/submissions + GET /api/submissions/{id}
//  - service   —— 业务服务
//  - jwt       —— 用于解析 Authorization Bearer
//  - is_db_ready —— DB 不可用时返回 503 envelope
void register_submission_routes(HttpServer& server,
                                 std::shared_ptr<oj::domain::ISubmissionService> service,
                                 std::shared_ptr<oj::infra::JwtService> jwt,
                                 std::function<bool()> is_db_ready);

}  // namespace oj::http::handlers
