#include "http/HttpServer.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "common/version.hpp"

namespace oj::http {

HttpServer::HttpServer(common::AppConfig config)
    : config_(std::move(config)),
      server_(std::make_unique<httplib::Server>()),
      started_at_(std::chrono::steady_clock::now()) {
    // 阶段 7 再接 access log；这里先用 spdlog 默认 pattern
}

HttpServer::~HttpServer() {
    if (server_ && server_->is_running()) {
        server_->stop();
    }
    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }
}

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    if (method == "GET") {
        server_->Get(path, std::move(handler));
    } else if (method == "POST") {
        server_->Post(path, std::move(handler));
    } else if (method == "PUT") {
        server_->Put(path, std::move(handler));
    } else if (method == "DELETE") {
        server_->Delete(path, std::move(handler));
    } else if (method == "PATCH") {
        server_->Patch(path, std::move(handler));
    } else {
        spdlog::warn("HttpServer::route: unsupported method {}", method);
    }
}

void HttpServer::install_exception_middleware() {
    server_->set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
            std::string what = "unknown";
            try {
                std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
                what = "non-std exception";
            }
            spdlog::error("unhandled exception in handler: {}", what);
            write_error(res, common::ErrorCode::Internal, "internal server error");
        });

    // 4xx/5xx 兜底：未注册路由或路由报错时统一返回 JSON envelope（与 SPEC §5.1 一致）
    // cpp-httplib v0.15 没有 set_404_handler；404 通过 set_error_handler 统一捕获
    server_->set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        spdlog::debug("error_handler: {} {} -> {}", req.method, req.path, res.status);
        if (!res.body.empty()) return;  // handler 已写过 body
        switch (res.status) {
            case 404:
                write_error(res, common::ErrorCode::NotFound,
                            "route not found: " + req.method + " " + req.path);
                break;
            case 405:
                write_error(res, common::ErrorCode::BadRequest,
                            "method not allowed: " + req.method + " " + req.path);
                break;
            case 400:
                write_error(res, common::ErrorCode::BadRequest, "bad request");
                break;
            case 401:
                write_error(res, common::ErrorCode::Unauthorized, "unauthorized");
                break;
            case 403:
                write_error(res, common::ErrorCode::Forbidden, "forbidden");
                break;
            case 413:
                write_error(res, common::ErrorCode::TooLarge, "payload too large");
                break;
            default:
                if (res.status >= 500) {
                    write_error(res, common::ErrorCode::Internal,
                                "internal server error");
                } else {
                    write_error(res, common::ErrorCode::BadRequest,
                                "request failed with status " + std::to_string(res.status));
                }
                break;
        }
    });
}

void HttpServer::install_logger(AccessLogHook hook) {
    if (!hook) return;
    server_->set_logger(std::move(hook));
}

void HttpServer::install_pre_routing(PreRoutingHook hook) {
    if (!hook) return;
    server_->set_pre_routing_handler(
        [fn = std::move(hook)](const httplib::Request& req,
                               httplib::Response& res) -> httplib::Server::HandlerResponse {
            return fn(req, res);
        });
}

void HttpServer::install_post_routing(PostRoutingHook hook) {
    if (!hook) return;
    server_->set_post_routing_handler(
        [fn = std::move(hook)](const httplib::Request& req,
                               httplib::Response& res) {
            fn(req, res);
        });
}

bool HttpServer::listen(std::string* reason) {
    install_exception_middleware();

    int threads = std::max(1, config_.server.thread_pool_size);
    server_->new_task_queue = [threads] {
        return new httplib::ThreadPool(threads);
    };

    // 绑定端口:
    //   - 若 config_.server.port == 0,改用 bind_to_any_port() 让 OS 分配,
    //     然后保存实际端口供 bound_port() 暴露(用于单测 port=0 的场景)
    //   - 否则按指定端口绑定
    if (config_.server.port == 0) {
        bound_port_ = server_->bind_to_any_port(config_.server.host);
        if (bound_port_ <= 0) {
            if (reason) {
                *reason = "httplib::Server::bind_to_any_port returned "
                          + std::to_string(bound_port_);
            }
            return false;
        }
        if (!server_->listen_after_bind()) {
            if (reason) {
                *reason = "httplib::Server::listen_after_bind returned false";
            }
            return false;
        }
    } else {
        if (!server_->listen(config_.server.host, config_.server.port)) {
            if (reason) {
                *reason = "httplib::Server::listen returned false";
            }
            return false;
        }
        bound_port_ = config_.server.port;
    }

    spdlog::info("oj_backend {} listening on {}:{} (threads={})",
                 OJ_VERSION_STRING,
                 config_.server.host,
                 bound_port_,
                 threads);
    return true;
}

bool HttpServer::start_async(std::string* reason) {
    // 与 listen() 共用绑定逻辑,但让 accept 循环跑在后台线程,
    // 立即返回 true。失败时返回 false 并写 reason。
    async_mode_ = true;
    listen_thread_ = std::thread([this, reason]() {
        // 在新线程里跑 listen();这里 listen() 会阻塞,直到 stop()
        if (!listen(reason)) {
            // listen 失败 → spdlog 已经写;此处不再重复
        }
    });
    // 等 server 真正 ready(避免测试在 bind 完成前就发请求)
    server_->wait_until_ready();
    return bound_port_ > 0;
}

int HttpServer::bound_port() const noexcept {
    return bound_port_;
}

void HttpServer::stop() {
    if (server_ && server_->is_running()) {
        server_->stop();
    }
    if (listen_thread_.joinable()) {
        listen_thread_.join();
    }
}

std::int64_t HttpServer::uptime_ms() const noexcept {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now() - started_at_).count();
}

}  // namespace oj::http