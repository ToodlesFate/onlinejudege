#pragma once

// =============================================================================
//  oj::http::handlers::admin_problem — 后台题目录入 / 编辑 / 上下架
//  SPEC §2.5 / §3.3.5 L,M / §5.2.4
//
//  路由一览：
//    GET    /api/admin/problems               列表（包含未发布）
//    POST   /api/admin/problems               新建（含 tags + cases）
//    GET    /api/admin/problems/{id}/edit-data 编辑表单数据（含全部 testcases）
//    PUT    /api/admin/problems/{id}          全量更新
//    DELETE /api/admin/problems/{id}          软删（is_published=0）
//    PATCH  /api/admin/problems/{id}/publish  上下架（body: {is_published: bool}）
//
//  鉴权：所有路由必须带 Authorization: Bearer <access_token>，且 is_admin=true
//        否则 → 1002 Unauthorized / 1003 Forbidden
//  DB：  不可用 → 1008 SystemError
//
//  注册方式：
//    register_admin_problem_routes(server, service, jwt, is_db_ready)
// =============================================================================

#include <functional>
#include <memory>

#include "domain/problem_service.hpp"
#include "http/HttpServer.hpp"  // 完整定义（register_admin_problem_routes 形参）
#include "infra/jwt_service.hpp"

namespace oj::http::handlers {

// 注册 /api/admin/problems* 路由
//  - service  —— 业务服务（IProblemService）
//  - jwt      —— 用于解析 Authorization Bearer
//  - is_db_ready —— DB 不可用时返回 503 envelope
void register_admin_problem_routes(
    HttpServer& server,
    std::shared_ptr<oj::domain::IProblemService>  service,
    std::shared_ptr<oj::infra::JwtService>        jwt,
    std::function<bool()>                         is_db_ready);

}  // namespace oj::http::handlers
