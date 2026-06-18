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

    // 注入 access log hook —— SPEC §2.6 / §9.3
    // 用法:
    //     server.install_logger([](const Request&, const Response&){ ... });
    // httplib::Logger = std::function<void(const Request&, const Response&)>
    using AccessLogHook = std::function<void(const httplib::Request&, const httplib::Response&)>;
    void install_logger(AccessLogHook hook);

    // 注入 pre-routing hook —— 在路由匹配前执行,可用于记 request 开始时间、
    // 鉴权、CORS 等。返回 Handled 会让 httplib 直接用 res.body 返回,跳过路由;
    // 返回 Unhandled / passthrough 表示交给后续路由匹配。
    using PreRoutingHook = std::function<httplib::Server::HandlerResponse(
        const httplib::Request&, httplib::Response&)>;
    void install_pre_routing(PreRoutingHook hook);

    // 注入 post-routing hook —— handler 跑完后、写入 socket 前执行。
    // 用途: 加安全响应头 / 统一埋点 / 灰度。
    using PostRoutingHook = std::function<void(const httplib::Request&, httplib::Response&)>;
    void install_post_routing(PostRoutingHook hook);

    // 阻塞监听；启动失败返回 false 并把错误写入 reason
    bool listen(std::string* reason = nullptr);

    // 非阻塞启动 (单测用): 与 listen() 等价的"绑端口 + 后台线程跑 accept 循环",
    // 立刻返回,失败时返回 false 并把原因写入 *reason。
    // 配合 stop() 在测试结束时清理。
    bool start_async(std::string* reason = nullptr);

    // 实际绑定的端口(用于单测 port=0 的场景)
    // listen 之前调用返回 config_.server.port;
    // listen 成功之后返回 OS 实际分配的端口。
    [[nodiscard]] int bound_port() const noexcept;

    // 停止监听（线程安全，可在 signal handler 中调用）
    void stop();

    // 进程启动到现在的毫秒数，供 /api/health 暴露 uptime
    [[nodiscard]] std::int64_t uptime_ms() const noexcept;

    [[nodiscard]] const common::AppConfig& config() const noexcept { return config_; }

private:
    common::AppConfig              config_;
    std::unique_ptr<httplib::Server> server_;
    std::chrono::steady_clock::time_point started_at_;
    int bound_port_{0};
    std::thread listen_thread_{};
    bool async_mode_{false};
};

}  // namespace oj::http