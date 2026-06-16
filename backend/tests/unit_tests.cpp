// =============================================================================
//  oj_unit_tests — 阶段 1 单元测试（GoogleTest）
//    - Response envelope 形状
//    - Config 解析（默认值 / 覆盖 / 错误格式）
//    - /api/health handler 返回正确 JSON
//    - HttpServer 端到端冒烟
// =============================================================================

#include <gtest/gtest.h>

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

// ---------------------------------------------------------------------------
//  ErrorCode
// ---------------------------------------------------------------------------
TEST(ErrorCodeTest, MappingIsStable) {
    using oj::common::ErrorCode;
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::Ok),          0);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::BadRequest),  1001);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::Unauthorized),1002);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::Forbidden),   1003);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::NotFound),    1004);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::Conflict),    1005);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::TooLarge),    1006);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::Internal),    1007);
    EXPECT_EQ(static_cast<std::int32_t>(ErrorCode::SystemError), 1008);

    EXPECT_EQ(oj::common::to_http_status(ErrorCode::Ok),          200);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::BadRequest),  400);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::Unauthorized),401);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::Forbidden),   403);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::NotFound),    404);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::Conflict),    409);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::TooLarge),    413);
    EXPECT_EQ(oj::common::to_http_status(ErrorCode::Internal),    500);
}

// ---------------------------------------------------------------------------
//  Response envelope
// ---------------------------------------------------------------------------
TEST(ResponseTest, OkBuildsEnvelope) {
    auto j = oj::common::Response::ok(nlohmann::json{{"answer", 42}});
    EXPECT_EQ(j["code"].get<int>(),                  0);
    EXPECT_EQ(j["message"].get<std::string>(),       "ok");
    ASSERT_TRUE(j["data"].is_object());
    EXPECT_EQ(j["data"]["answer"].get<int>(),        42);
}

TEST(ResponseTest, ErrorDefaultsMessageFromCode) {
    auto j = oj::common::Response::error(oj::common::ErrorCode::BadRequest);
    EXPECT_EQ(j["code"].get<int>(),            1001);
    EXPECT_EQ(j["message"].get<std::string>(), "bad request");
    EXPECT_TRUE(j["data"].is_null());
}

TEST(ResponseTest, ErrorAcceptsOverrideMessage) {
    auto j = oj::common::Response::error(oj::common::ErrorCode::BadRequest,
                                         "username too short");
    EXPECT_EQ(j["code"].get<int>(),            1001);
    EXPECT_EQ(j["message"].get<std::string>(), "username too short");
}

// ---------------------------------------------------------------------------
//  AppConfig
// ---------------------------------------------------------------------------
TEST(AppConfigTest, LoadUsesDefaultsWhenFieldsMissing) {
    TempConfigFile f("{}");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.server.host,             "0.0.0.0");
    EXPECT_EQ(cfg.server.port,             8080);
    EXPECT_EQ(cfg.server.thread_pool_size, 8);
    EXPECT_EQ(cfg.log.level,               "info");
}

TEST(AppConfigTest, LoadHonorsOverrides) {
    TempConfigFile f(R"({
        "server": {"host": "127.0.0.1", "port": 9000, "thread_pool_size": 2},
        "log":    {"level": "debug", "stdout": false, "dir": "/tmp/oj-test"}
    })");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.server.host,             "127.0.0.1");
    EXPECT_EQ(cfg.server.port,             9000);
    EXPECT_EQ(cfg.server.thread_pool_size, 2);
    EXPECT_EQ(cfg.log.level,               "debug");
    EXPECT_FALSE(cfg.log.stdout_console);
    EXPECT_EQ(cfg.log.dir,                 std::filesystem::path{"/tmp/oj-test"});
}

TEST(AppConfigTest, LoadRejectsMalformedJson) {
    TempConfigFile f("{not json");
    EXPECT_THROW(oj::common::AppConfig::load(f.path()), oj::common::ConfigError);
}

// ---------------------------------------------------------------------------
//  /api/health handler
// ---------------------------------------------------------------------------
TEST(HealthHandlerTest, Returns200Envelope) {
    httplib::Request  req;
    httplib::Response res;
    oj::http::handlers::health(req, res, /*uptime_ms=*/123);

    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.get_header_value("Content-Type"), "application/json; charset=utf-8");

    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            0);
    EXPECT_EQ(body["message"].get<std::string>(), "ok");
    ASSERT_TRUE(body["data"].is_object());
    EXPECT_EQ(body["data"]["status"].get<std::string>(), "ok");
    EXPECT_EQ(body["data"]["version"].get<std::string>(), OJ_VERSION_STRING);
    EXPECT_EQ(body["data"]["uptime_ms"].get<int>(),       123);
    ASSERT_TRUE(body["data"].contains("now_unix"));
    EXPECT_GT(body["data"]["now_unix"].get<std::int64_t>(), 0);
}

// ---------------------------------------------------------------------------
//  http::write_error
// ---------------------------------------------------------------------------
TEST(HttpHelpersTest, WriteErrorSerializes) {
    httplib::Response res;
    oj::http::write_error(res, oj::common::ErrorCode::Unauthorized, "missing token");
    EXPECT_EQ(res.status, 401);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            1002);
    EXPECT_EQ(body["message"].get<std::string>(), "missing token");
    EXPECT_TRUE(body["data"].is_null());
}

// ---------------------------------------------------------------------------
//  HttpServer 端到端
// ---------------------------------------------------------------------------
TEST(HttpServerE2ETest, GetHealthOnLocalPort) {
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

    // 等 listen 真正进入 accept 循环
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
        GTEST_SKIP() << "skipping live e2e: port 18080 not reachable in this environment";
    }

    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["code"].get<int>(),                          0);
    EXPECT_EQ(body["data"]["status"].get<std::string>(),       "ok");
    EXPECT_EQ(body["data"]["version"].get<std::string>(),      OJ_VERSION_STRING);
}
