#pragma once

#include <cstdint>

#include <httplib.h>

namespace oj::http::handlers {

// GET /api/health —— 用于 Docker HEALTHCHECK 与负载探针
// 始终返回 200 + envelope data={status:"ok", version, uptime_ms, now_unix}
void health(const httplib::Request& req, httplib::Response& res, std::int64_t uptime_ms);

}  // namespace oj::http::handlers