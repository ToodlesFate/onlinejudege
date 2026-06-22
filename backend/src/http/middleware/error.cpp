#include "http/middleware/error.hpp"

#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "http/HttpServer.hpp"  // write_error / write_ok 完整定义

namespace oj::http::middleware {

// -----------------------------------------------------------------------------
//  wrap_handler
// -----------------------------------------------------------------------------
Handler wrap_handler(Handler inner) {
    return [fn = std::move(inner)](const httplib::Request& req,
                                   httplib::Response&      res) {
        try {
            fn(req, res);
            return;
        } catch (const HttpError& e) {
            // 业务层主动抛出的"预期"错误 —— 4xx 走 warn 级别日志
            // (便于运维快速过滤"非系统故障"日志,排查逻辑 bug 用)
            spdlog::warn("HttpError in handler (code={}): {}",
                         static_cast<int>(e.code()), e.what());
            // 若 handler 已经写过 body (例如 set_content 后又抛了) ,
            // write_error 内部会覆盖,保持 envelope 形状不被破坏。
            write_error(res, e.code(), e.what());
        } catch (const std::exception& e) {
            // 业务逻辑或基础设施抛的"非预期"异常 —— 5xx 走 error 级别
            // 原始 e.what() 只写日志,不对外暴露(避免泄漏内部细节)
            spdlog::error("unhandled std::exception in handler: {}", e.what());
            write_error(res, oj::common::ErrorCode::Internal,
                        "internal server error");
        } catch (...) {
            spdlog::error("unhandled non-std exception in handler");
            write_error(res, oj::common::ErrorCode::Internal,
                        "internal server error");
        }
    };
}

// -----------------------------------------------------------------------------
//  check_db_ready
// -----------------------------------------------------------------------------
bool check_db_ready(httplib::Response& res, const std::function<bool()>& is_db_ready) {
    if (is_db_ready && !is_db_ready()) {
        // 与 middleware.cpp::db_unavailable_response 等价;为减少依赖跳数,
        // 直接调 helper。
        write_error(res, oj::common::ErrorCode::SystemError,
                    "database not available");
        return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
//  parse_path_id
// -----------------------------------------------------------------------------
std::optional<std::int64_t> parse_path_id(const httplib::Request& req,
                                          std::string_view         name) {
    // cpp-httplib 用 std::string 当 key;需要从 string_view 构造临时 key
    // 注意 path_params 自身就是 std::string 容器,所以这里只能用 string 查
    const auto it = req.path_params.find(std::string{name});
    if (it == req.path_params.end()) return std::nullopt;

    const std::string& s = it->second;
    if (s.empty()) return std::nullopt;

    try {
        std::size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos != s.size()) return std::nullopt;  // 含非数字字符
        if (v <= 0)          return std::nullopt;  // 非正数
        return static_cast<std::int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// -----------------------------------------------------------------------------
//  parse_query_int
// -----------------------------------------------------------------------------
std::optional<std::int64_t> parse_query_int(const httplib::Request& req,
                                             std::string_view         name,
                                             QueryIntOptions          opts) {
    if (!req.has_param(std::string{name})) return std::nullopt;
    const std::string& s = req.get_param_value(std::string{name});
    if (s.empty()) return std::nullopt;

    try {
        std::size_t pos = 0;
        const long long v = std::stoll(s, &pos);
        if (pos != s.size()) return std::nullopt;  // 含非数字字符
        if (opts.min_value && v < *opts.min_value) return std::nullopt;
        if (opts.max_value && v > *opts.max_value) return std::nullopt;
        return static_cast<std::int64_t>(v);
    } catch (...) {
        return std::nullopt;
    }
}

// -----------------------------------------------------------------------------
//  require_string_field
// -----------------------------------------------------------------------------
std::string require_string_field(const nlohmann::json& body, std::string_view name) {
    const auto it = body.find(std::string{name});
    if (it == body.end() || it->is_null()) {
        throw HttpError::bad_request(
            std::string{"missing required field: "} + std::string{name});
    }
    if (!it->is_string()) {
        throw HttpError::bad_request(
            std::string{"field must be a string: "} + std::string{name});
    }
    return it->get<std::string>();
}

}  // namespace oj::http::middleware
