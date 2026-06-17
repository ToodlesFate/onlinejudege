#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "common/response.hpp"

namespace oj::http {

// 与 cpp-httplib 同形 handler —— SPEC §3.2.2 路由表注册用
using Handler     = std::function<void(const httplib::Request&, httplib::Response&)>;
using RouteParams = httplib::Request;

// JSON 写入助手，统一 Content-Type 和 HTTP 状态码
inline void write_json(httplib::Response& res,
                       common::ErrorCode code,
                       nlohmann::json    data = nullptr,
                       std::string       message = {}) {
    if (message.empty()) {
        message = std::string{common::to_string(code)};
    }
    nlohmann::json body = {
        {"code",    static_cast<std::int32_t>(code)},
        {"message", std::move(message)},
        {"data",    std::move(data)},
    };
    res.status = common::to_http_status(code);
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

inline void write_ok(httplib::Response& res, nlohmann::json data = nullptr) {
    write_json(res, common::ErrorCode::Ok, std::move(data), "ok");
}

inline void write_error(httplib::Response& res,
                        common::ErrorCode  code,
                        std::string        message = {}) {
    write_json(res, code, nullptr, std::move(message));
}

// HttpServer —— SPEC §3.2.2：包装 cpp-httplib，绑定监听、注册路由、挂中间件
class HttpServer {
public:
    explicit HttpServer(common::AppConfig config);
    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // 注册 handler（method 全大写，如 "GET"/"POST"）
    void route(const std::string& method, const std::string& path, Handler handler);

    void get   (const std::string& path, Handler h) { route("GET",    path, std::move(h)); }
    void post  (const std::string& path, Handler h) { route("POST",   path, std::move(h)); }
    void put   (const std::string& path, Handler h) { route("PUT",    path, std::move(h)); }
    void del   (const std::string& path, Handler h) { route("DELETE", path, std::move(h)); }
    void patch (const std::string& path, Handler h) { route("PATCH",  path, std::move(h)); }

    // 安装全局异常中间件：handler 抛异常时统一返回 500 envelope
    void install_exception_middleware();

    // 阻塞监听；启动失败返回 false 并把错误写入 reason
    bool listen(std::string* reason = nullptr);

    // 停止监听（线程安全，可在 signal handler 中调用）
    void stop();

    // 进程启动到现在的毫秒数，供 /api/health 暴露 uptime
    [[nodiscard]] std::int64_t uptime_ms() const noexcept;

    [[nodiscard]] const common::AppConfig& config() const noexcept { return config_; }

private:
    common::AppConfig              config_;
    std::unique_ptr<httplib::Server> server_;
    std::chrono::steady_clock::time_point started_at_;
};

}  // namespace oj::http