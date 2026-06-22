#pragma once

// =============================================================================
//  oj::http::middleware::error — 统一错误中间件 (SPEC §3.2.2)
//
//  本文件实现"统一错误中间件"的核心 API。设计目标:
//
//   1. 单一错误响应路径:handler 抛 HttpError → 中间件捕获 → 写 envelope。
//      不再依赖每个 handler 手动 try/catch + write_error。
//
//   2. 不破坏现有风格:继续支持 handler 直接调 write_error/write_ok,
//      wrap_handler 是 opt-in。
//
//   3. 类型安全:HttpError 携带 ErrorCode 枚举,编译期检查;不再用 magic int。
//
//   4. 集中日志:HttpError 走 spdlog::warn(预期内的 4xx 业务错误),
//      std::exception 走 spdlog::error(未预期的 5xx 系统错误),
//      运维一眼能区分业务拒绝 vs 系统故障。
//
//  典型用法(新 handler):
//
//      server.post("/api/foo", mw::wrap_handler(
//          [&](const httplib::Request& req, httplib::Response& res) {
//              if (req.body.empty())
//                  throw mw::HttpError::bad_request("empty body");
//              auto id = mw::parse_path_id(req, "id");
//              if (!id) throw mw::HttpError::bad_request("id must be int");
//              // ... 业务逻辑
//              write_ok(res, {{"id", *id}});
//          }));
//
//  handler 异常时的中间件行为:
//    - HttpError(code, msg) → write_error(res, code, msg),spdlog::warn
//    - std::exception&    → write_error(res, Internal, "internal server error"),
//                           spdlog::error(原 .what() 写入日志但不对外暴露)
//    - 其它(...)          → write_error(res, Internal, "internal server error"),
//                           spdlog::error("non-std exception")
//
//  与 SPEC §5.1 错误码表对齐:
//    1001 BadRequest / 1002 Unauthorized / 1003 Forbidden / 1004 NotFound
//    1005 Conflict / 1006 TooLarge / 1007 Internal / 1008 SystemError
// =============================================================================

#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/error_code.hpp"

namespace oj::http {

class HttpServer;  // forward declaration for install function

}  // namespace oj::http

namespace oj::http::middleware {

// -----------------------------------------------------------------------------
//  HttpError —— 业务层主动抛出的"协议级"错误。
//  继承 std::runtime_error 是为了 catch (const std::exception&) 也能 catch 住;
//  额外携带 ErrorCode 让中间件能精确翻译成 4xx/5xx + envelope。
// -----------------------------------------------------------------------------
class HttpError : public std::runtime_error {
public:
    HttpError(oj::common::ErrorCode code, std::string message)
        : std::runtime_error(message.empty()
                                 ? std::string{oj::common::to_string(code)}
                                 : std::move(message)),
          code_(code) {}

    [[nodiscard]] oj::common::ErrorCode code() const noexcept { return code_; }

    // 工厂方法:让 handler 写法更紧凑。
    //   throw HttpError::bad_request("xxx");
    //   throw HttpError::not_found();
    [[nodiscard]] static HttpError bad_request(std::string msg = "bad request") {
        return HttpError(oj::common::ErrorCode::BadRequest, std::move(msg));
    }
    [[nodiscard]] static HttpError unauthorized(std::string msg = "unauthorized") {
        return HttpError(oj::common::ErrorCode::Unauthorized, std::move(msg));
    }
    [[nodiscard]] static HttpError forbidden(std::string msg = "forbidden") {
        return HttpError(oj::common::ErrorCode::Forbidden, std::move(msg));
    }
    [[nodiscard]] static HttpError not_found(std::string msg = "not found") {
        return HttpError(oj::common::ErrorCode::NotFound, std::move(msg));
    }
    [[nodiscard]] static HttpError conflict(std::string msg) {
        return HttpError(oj::common::ErrorCode::Conflict, std::move(msg));
    }
    [[nodiscard]] static HttpError too_large(std::string msg = "payload too large") {
        return HttpError(oj::common::ErrorCode::TooLarge, std::move(msg));
    }
    [[nodiscard]] static HttpError internal(std::string msg = "internal server error") {
        return HttpError(oj::common::ErrorCode::Internal, std::move(msg));
    }
    [[nodiscard]] static HttpError system_error(std::string msg = "system error") {
        return HttpError(oj::common::ErrorCode::SystemError, std::move(msg));
    }

private:
    oj::common::ErrorCode code_;
};

// -----------------------------------------------------------------------------
//  wrap_handler —— 错误中间件的核心。
//
//  接受任意 handler (callable),返回一个新的 cpp-httplib 兼容 handler。
//  新 handler 会在调用原 handler 时统一 catch 异常并写 envelope。
//
//  注意:cpp-httplib 的 Handler 是 std::function<void(const Request&, Response&)>,
//  所以本函数的返回类型与之一致;不强制使用模板形参的推导,
//  直接用 std::function 包装避免 lambda capture 类型擦除问题。
// -----------------------------------------------------------------------------
using Handler = std::function<void(const httplib::Request&, httplib::Response&)>;
Handler wrap_handler(Handler inner);

// -----------------------------------------------------------------------------
//  check_db_ready —— "DB 不可用"统一兜底。
//  返回 true 表示 handler 可继续执行;返回 false 表示已写 1008 envelope。
//  与原 middleware.cpp::db_unavailable_response 行为一致,封装为"判断+写"二合一。
// -----------------------------------------------------------------------------
bool check_db_ready(httplib::Response& res, const std::function<bool()>& is_db_ready);

// -----------------------------------------------------------------------------
//  parse_path_id —— 解析 :id 这类路径参数,返回 optional<int64_t>。
//
//  返回 std::nullopt 的三种情况:
//    - 路径参数不存在
//    - 非整数 / 含非数字字符
//    - 整数 <= 0
//
//  handler 用法:
//      auto id = parse_path_id(req, "id");
//      if (!id) throw HttpError::bad_request("id must be a positive integer");
//      ...
// -----------------------------------------------------------------------------
std::optional<std::int64_t> parse_path_id(const httplib::Request& req,
                                          std::string_view         name);

// -----------------------------------------------------------------------------
//  parse_query_int —— 解析 ?key=123 类 query 参数为 optional<int64_t>。
//
//  返回 std::nullopt 的三种情况:
//    - 参数不存在 / 空字符串
//    - 非整数 / 含非数字字符
//    - min/max 范围越界(可选;min > max 时忽略 max)
//
//  注:返回 0 在 spec 里通常代表"缺省"或"无限制",所以 0 永远合法。
// -----------------------------------------------------------------------------
struct QueryIntOptions {
    std::optional<std::int64_t> min_value;  // inclusive lower bound
    std::optional<std::int64_t> max_value;  // inclusive upper bound
};

std::optional<std::int64_t> parse_query_int(const httplib::Request& req,
                                             std::string_view         name,
                                             QueryIntOptions          opts = {});

// -----------------------------------------------------------------------------
//  require_string_field —— 提取 string 字段,缺/非 string 抛 1001。
//  handler 写 GET-style 接口时省掉一个 if 块。
// -----------------------------------------------------------------------------
std::string require_string_field(const nlohmann::json& body, std::string_view name);

}  // namespace oj::http::middleware
