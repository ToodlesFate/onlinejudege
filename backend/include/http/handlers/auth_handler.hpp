#pragma once

// =============================================================================
//  oj::http::handlers::auth — 注册 / 登录 / 刷新 HTTP 入口
//  本阶段仅实现 POST /api/auth/register；其他端点见后续 Phase 2 子项。
//
//  handler 全部以 free function 形式暴露（与 health_handler 风格一致），
//  由 main.cpp 注入 AuthService 共享指针 + DB readiness 检查回调。
// =============================================================================

#include <functional>
#include <memory>

#include "domain/auth_service.hpp"
#include "http/HttpServer.hpp"  // 完整定义（register_auth_routes 形参）

namespace oj::http::handlers {

// 注册 POST /api/auth/register 的路由。
// 绑定的 auth / is_db_ready 生命周期须 ≥ HttpServer 生命周期（main.cpp 持有 shared_ptr）。
//
// `is_db_ready` —— 在 MySQL 未就绪时返回 false；handler 收到 false 时返回 503 envelope。
//               用 std::function 而非 shared_ptr<MysqlClient>，避免 http 层依赖 infra 层具体类型。
void register_auth_routes(HttpServer& server,
                          std::shared_ptr<oj::domain::AuthService> auth,
                          std::function<bool()> is_db_ready);

}  // namespace oj::http::handlers
