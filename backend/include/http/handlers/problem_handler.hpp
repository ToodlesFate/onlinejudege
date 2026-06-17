#pragma once

// =============================================================================
//  oj::http::handlers::problem —— 题目相关 HTTP 入口
//  本阶段实现：
//    - GET /api/problems        公开列表 (分页/过滤/排序) —— SPEC §5.2.2
//    - GET /api/problems/:id    公开详情 (题面 + 样例点) —— SPEC §5.2.2
//    - GET /api/tags            公开预置标签列表    —— SPEC §5.2.2
//
//  后续阶段（本任务范围外）：
//    - GET /api/admin/problems
//    - POST /api/admin/problems
//    - PUT  /api/admin/problems/{id}
//    - DELETE /api/admin/problems/{id}
//    - PATCH /api/admin/problems/{id}/publish
//
//  注册方式：调用 register_problem_routes(server, problems, is_db_ready)
//  注入 IProblemService（不是 repo）—— handler 不应直接依赖持久化
// =============================================================================

#include <functional>
#include <memory>

#include "domain/problem_service.hpp"
#include "http/HttpServer.hpp"  // 完整定义（register_problem_routes 形参）

namespace oj::http::handlers {

// 注册 GET /api/problems 路由
//  - service  —— 列表查询服务（业务校验 + 委托 repo）
//  - is_db_ready —— MySQL 不可用时返回 503（SPEC §2.6 可用性）
void register_problem_routes(HttpServer& server,
                             std::shared_ptr<oj::domain::IProblemService> service,
                             std::function<bool()> is_db_ready);

}  // namespace oj::http::handlers
