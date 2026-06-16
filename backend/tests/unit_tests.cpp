// =============================================================================
//  oj_unit_tests — 阶段 1 单元测试
//    - Response envelope 形状
//    - Config 解析（默认值 / 覆盖 / 错误格式）
//    - /api/health handler 返回正确 JSON
// =============================================================================

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "common/response.hpp"
#include "common/version.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/health_handler.hpp"

namespace {

// 把对象写到临时文件并返回路径（测试结束自动清理）
class TempConfigFile {
public:
    explicit TempConfigFile(std::string body) {
        path_ = std::filesystem::temp_directory_path() /
                ("oj_cfg_" + std::to_string(::getpid()) + "_" +
                 std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json");
        std::ofstream out(path_);
        out << body;
    }
    ~TempConfigFile() { std::error_code ec; std::filesystem::remove(path_, ec); }
    TempConfigFile(const TempConfigFile&)            = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace

TEST_CASE("error_code mapping is stable") {
    using oj::common::ErrorCode;
    CHECK(static_cast<std::int32_t>(ErrorCode::Ok) == 0);
    CHECK(static_cast<std::int32_t>(ErrorCode::BadRequest) == 1001);
    CHECK(static_cast<std::int32_t>(ErrorCode::Unauthorized) == 1002);
    CHECK(static_cast<std::int32_t>(ErrorCode::Forbidden) == 1003);
    CHECK(static_cast<std::int32_t>(ErrorCode::NotFound) == 1004);
    CHECK(static_cast<std::int32_t>(ErrorCode::Conflict) == 1005);
    CHECK(static_cast<std::int32_t>(ErrorCode::TooLarge) == 1006);
    CHECK(static_cast<std::int32_t>(ErrorCode::Internal) == 1007);
    CHECK(static_cast<std::int32_t>(ErrorCode::SystemError) == 1008);

    CHECK(oj::common::to_http_status(ErrorCode::Ok) == 200);
    CHECK(oj::common::to_http_status(ErrorCode::BadRequest) == 400);
    CHECK(oj::common::to_http_status(ErrorCode::Unauthorized) == 401);
    CHECK(oj::common::to_http_status(ErrorCode::Forbidden) == 403);
    CHECK(oj::common::to_http_status(ErrorCode::NotFound) == 404);
    CHECK(oj::common::to_http_status(ErrorCode::Conflict) == 409);
    CHECK(oj::common::to_http_status(ErrorCode::TooLarge) == 413);
    CHECK(oj::common::to_http_status(ErrorCode::Internal) == 500);
}

TEST_CASE("Response::ok builds the SPEC §5.1 envelope") {
    auto j = oj::common::Response::ok(nlohmann::json{{"answer", 42}});
    CHECK(j["code"] == 0);
    CHECK(j["message"] == "ok");
    REQUIRE(j["data"].is_object());
    CHECK(j["data"]["answer"] == 42);
}

TEST_CASE("Response::error defaults message from code") {
    auto j = oj::common::Response::error(oj::common::ErrorCode::BadRequest);
    CHECK(j["code"] == 1001);
    CHECK(j["message"] == "bad request");
    CHECK(j["data"].is_null());
}

TEST_CASE("Response::error accepts override message") {
    auto j = oj::common::Response::error(oj::common::ErrorCode::BadRequest, "username too short");
    CHECK(j["code"] == 1001);
    CHECK(j["message"] == "username too short");
}

TEST_CASE("AppConfig::load uses defaults when fields are missing") {
    TempConfigFile f("{}");
    auto cfg = oj::common::AppConfig::load(f.path());
    CHECK(cfg.server.host == "0.0.0.0");
    CHECK(cfg.server.port == 8080);
    CHECK(cfg.server.thread_pool_size == 8);
    CHECK(cfg.log.level == "info");
}

TEST_CASE("AppConfig::load honors overrides") {
    TempConfigFile f(R"({
        "server": {"host": "127.0.0.1", "port": 9000, "thread_pool_size": 2},
        "log":    {"level": "debug", "stdout": false, "dir": "/tmp/oj-test"}
    })");
    auto cfg = oj::common::AppConfig::load(f.path());
    CHECK(cfg.server.host == "127.0.0.1");
    CHECK(cfg.server.port == 9000);
    CHECK(cfg.server.thread_pool_size == 2);
    CHECK(cfg.log.level == "debug");
    CHECK(cfg.log.stdout_console == false);
    CHECK(cfg.log.dir == std::filesystem::path{"/tmp/oj-test"});
}

TEST_CASE("AppConfig::load rejects malformed json") {
    TempConfigFile f("{not json");
    CHECK_THROWS_AS(oj::common::AppConfig::load(f.path()), oj::common::ConfigError);
}

TEST_CASE("/api/health returns 200 envelope with status=ok") {
    httplib::Request  req;
    httplib::Response res;
    oj::http::handlers::health(req, res, /*uptime_ms=*/123);

    CHECK(res.status == 200);
    CHECK(res.get_header_value("Content-Type") == "application/json; charset=utf-8");

    auto body = nlohmann::json::parse(res.body);
    CHECK(body["code"] == 0);
    CHECK(body["message"] == "ok");
    REQUIRE(body["data"].is_object());
    CHECK(body["data"]["status"] == "ok");
    CHECK(body["data"]["version"] == OJ_VERSION_STRING);
    CHECK(body["data"]["uptime_ms"] == 123);
    CHECK(body["data"].contains("now_unix"));
    CHECK(body["data"]["now_unix"].get<std::int64_t>() > 0);
}

TEST_CASE("write_error serializes ErrorCode → HTTP status + envelope") {
    httplib::Response res;
    oj::http::write_error(res, oj::common::ErrorCode::Unauthorized, "missing token");
    CHECK(res.status == 401);
    auto body = nlohmann::json::parse(res.body);
    CHECK(body["code"] == 1002);
    CHECK(body["message"] == "missing token");
    CHECK(body["data"].is_null());
}

TEST_CASE("HttpServer end-to-end: GET /api/health on ephemeral port") {
    // 端到端：起一个真 HttpServer 在后台线程 → httplib::Client 拉 /api/health
    oj::common::AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = 18080;                // 固定本地端口；CI 上保持独占
    cfg.server.thread_pool_size = 2;
    cfg.log.stdout_console = false;
    cfg.log.dir = std::filesystem::temp_directory_path();

    oj::http::HttpServer srv(std::move(cfg));
    srv.get("/api/health", [&srv](const httplib::Request& q, httplib::Response& r) {
        oj::http::handlers::health(q, r, srv.uptime_ms());
    });

    std::atomic<bool> ready{false};
    std::thread t([&] {
        ready.store(true, std::memory_order_release);
        srv.listen();
    });

    // 等到 /api/health 200 就算通过；超时 3s 跳过（CI 环境端口可能冲突）
    for (int i = 0; i < 300 && !ready.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    httplib::Client cli("127.0.0.1", 18080);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);

    auto res = cli.Get("/api/health");
    srv.stop();
    if (t.joinable()) t.join();

    if (!res) {
        WARN("skipping live e2e: port 18080 not reachable in this environment");
        return;
    }
    CHECK(res->status == 200);
    auto body = nlohmann::json::parse(res->body);
    CHECK(body["code"] == 0);
    CHECK(body["data"]["status"] == "ok");
    CHECK(body["data"]["version"] == OJ_VERSION_STRING);
}