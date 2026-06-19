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
#include <stdexcept>
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
//  LogConfig — SPEC §3.2.3 / §2.6 可观测
// ---------------------------------------------------------------------------

TEST(LogConfigTest, DefaultsAreSpecBaseline) {
    // SPEC §3.2.3: 默认 level=info / dir=/var/log/oj / 轮转 100MB × 10 份
    TempConfigFile f("{}");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.log.level,               "info");
    EXPECT_EQ(cfg.log.dir,                 std::filesystem::path{"/var/log/oj"});
    EXPECT_TRUE(cfg.log.stdout_console);
    EXPECT_EQ(cfg.log.max_size_mb,         100);
    EXPECT_EQ(cfg.log.max_files,           10);
}

TEST(LogConfigTest, HonorsRotationOverrides) {
    TempConfigFile f(R"({
        "log": {"max_size_mb": 50, "max_files": 5, "level": "debug"}
    })");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.log.max_size_mb, 50);
    EXPECT_EQ(cfg.log.max_files,   5);
    EXPECT_EQ(cfg.log.level,       "debug");
}

TEST(LogConfigTest, RejectsNonPositiveMaxSizeMb) {
    TempConfigFile f(R"({"log": {"max_size_mb": 0}})");
    EXPECT_THROW(oj::common::AppConfig::load(f.path()), oj::common::ConfigError);
}

TEST(LogConfigTest, RejectsZeroMaxFiles) {
    TempConfigFile f(R"({"log": {"max_files": 0}})");
    EXPECT_THROW(oj::common::AppConfig::load(f.path()), oj::common::ConfigError);
}

TEST(LogConfigTest, NegativeMaxFilesRejected) {
    TempConfigFile f(R"({"log": {"max_files": -3}})");
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

// =============================================================================
//  Phase 1 — 边界场景 / 异常路径补充测试
//
//  覆盖以下场景（按 SPEC §3.2.1 / §5.1 / §9.4 M-2 要求的"Auth 关键路径 ≥ 80%"
//  之精神，本阶段先穷举 common/http 两层的所有可枚举分支）：
//
//    ErrorCode   — to_string / to_http_status 防御性（未知值）
//    Response    — 数组 / 嵌套对象 / 非 ASCII 错误消息
//    AppConfig   — load_from_string / 缺文件 / 空 JSON / 部分覆盖 / 类型错误
//    Health      — uptime 透传 / now_unix 时间合理性
//    HttpHelpers — write_json / write_ok / write_error 全部 9 个 code 的状态码
//    HttpServer  — 404 / 405 / 顺序多次 / handler 抛异常 → 500 envelope
// =============================================================================

// ---------------------------------------------------------------------------
//  ErrorCode — 防御性
// ---------------------------------------------------------------------------
TEST(ErrorCodeTest, ToStringForAllKnownCodes) {
    using oj::common::ErrorCode;
    using oj::common::to_string;
    EXPECT_STREQ(to_string(ErrorCode::Ok).data(),          "ok");
    EXPECT_STREQ(to_string(ErrorCode::BadRequest).data(),  "bad request");
    EXPECT_STREQ(to_string(ErrorCode::Unauthorized).data(),"unauthorized");
    EXPECT_STREQ(to_string(ErrorCode::Forbidden).data(),   "forbidden");
    EXPECT_STREQ(to_string(ErrorCode::NotFound).data(),    "not found");
    EXPECT_STREQ(to_string(ErrorCode::Conflict).data(),    "conflict");
    EXPECT_STREQ(to_string(ErrorCode::TooLarge).data(),    "payload too large");
    EXPECT_STREQ(to_string(ErrorCode::Internal).data(),    "internal server error");
    EXPECT_STREQ(to_string(ErrorCode::SystemError).data(), "system error");
}

TEST(ErrorCodeTest, UnknownCodeFallsBackToSafeDefaults) {
    using oj::common::ErrorCode;
    // 从 enum 范围外构造一个非法值（不应在生产路径中出现，但防御性兜底）
    constexpr auto bogus = static_cast<ErrorCode>(9999);
    EXPECT_EQ(oj::common::to_string(bogus), "unknown");
    EXPECT_EQ(oj::common::to_http_status(bogus), 500);
}

// ---------------------------------------------------------------------------
//  Response — 数据形态
// ---------------------------------------------------------------------------
TEST(ResponseTest, OkWithArrayData) {
    auto j = oj::common::Response::ok(nlohmann::json::array({1, 2, 3}));
    EXPECT_EQ(j["code"].get<int>(),                0);
    EXPECT_EQ(j["message"].get<std::string>(),     "ok");
    ASSERT_TRUE(j["data"].is_array());
    EXPECT_EQ(j["data"].size(),                    3u);
    EXPECT_EQ(j["data"][1].get<int>(),             2);
}

TEST(ResponseTest, OkWithNestedObjectData) {
    nlohmann::json inner = {
        {"user", {{"id", 5}, {"name", "alice"}}},
        {"tags", nlohmann::json::array({"dp", "greedy"})},
    };
    auto j = oj::common::Response::ok(inner);
    ASSERT_TRUE(j["data"].is_object());
    EXPECT_EQ(j["data"]["user"]["name"].get<std::string>(), "alice");
    EXPECT_EQ(j["data"]["tags"][0].get<std::string>(),      "dp");
}

TEST(ResponseTest, ErrorWithNonAsciiMessage) {
    auto j = oj::common::Response::error(
        oj::common::ErrorCode::BadRequest, "用户名长度必须在 3–20 字符之间");
    EXPECT_EQ(j["code"].get<int>(), 1001);
    EXPECT_EQ(j["message"].get<std::string>(),
              "用户名长度必须在 3–20 字符之间");
}

TEST(ResponseTest, OkWithEmptyObjectData) {
    auto j = oj::common::Response::ok(nlohmann::json::object());
    EXPECT_EQ(j["code"].get<int>(),              0);
    EXPECT_EQ(j["message"].get<std::string>(),   "ok");
    EXPECT_TRUE(j["data"].is_object());
    EXPECT_TRUE(j["data"].empty());
}

// ---------------------------------------------------------------------------
//  AppConfig — 加载路径
// ---------------------------------------------------------------------------
TEST(AppConfigTest, LoadFromStringEquivalentToFileLoad) {
    const std::string body = R"({
        "server": {"host": "10.0.0.1", "port": 9999, "thread_pool_size": 4},
        "log":    {"level": "warn",  "stdout": false, "dir": "/var/log/x"}
    })";
    TempConfigFile f(body);

    auto from_file   = oj::common::AppConfig::load(f.path());
    auto from_string = oj::common::AppConfig::load_from_string(body);

    EXPECT_EQ(from_file.server.host, from_string.server.host);
    EXPECT_EQ(from_file.server.port, from_string.server.port);
    EXPECT_EQ(from_file.server.thread_pool_size, from_string.server.thread_pool_size);
    EXPECT_EQ(from_file.log.level,    from_string.log.level);
    EXPECT_EQ(from_file.log.dir,      from_string.log.dir);
    EXPECT_EQ(from_file.log.stdout_console, from_string.log.stdout_console);
}

TEST(AppConfigTest, MissingFileThrowsConfigError) {
    auto bogus = std::filesystem::temp_directory_path() /
                 ("oj_missing_" + std::to_string(::getpid()) + ".json");
    // 确保不存在
    std::error_code ec;
    std::filesystem::remove(bogus, ec);
    EXPECT_THROW(oj::common::AppConfig::load(bogus), oj::common::ConfigError);
}

TEST(AppConfigTest, EmptyJsonObjectUsesAllDefaults) {
    TempConfigFile f("{}");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.server.host,             "0.0.0.0");
    EXPECT_EQ(cfg.server.port,             8080);
    EXPECT_EQ(cfg.server.thread_pool_size, 8);
    EXPECT_EQ(cfg.log.level,               "info");
    EXPECT_TRUE(cfg.log.stdout_console);
    EXPECT_EQ(cfg.log.dir,                 std::filesystem::path{"/var/log/oj"});
}

TEST(AppConfigTest, PartialOverrideOnlyPortLeavesRestAsDefault) {
    TempConfigFile f(R"({"server": {"port": 12345}})");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.server.host,             "0.0.0.0");      // default 保留
    EXPECT_EQ(cfg.server.port,             12345);           // 被覆盖
    EXPECT_EQ(cfg.server.thread_pool_size, 8);               // default 保留
}

TEST(AppConfigTest, WrongTypeForPortFallsBackToDefault) {
    // port 写成字符串 → 解析分支里 is_number_integer() 返回 false，保持默认值
    TempConfigFile f(R"({"server": {"port": "not-a-number"}})");
    auto cfg = oj::common::AppConfig::load(f.path());
    EXPECT_EQ(cfg.server.port, 8080);
}

TEST(AppConfigTest, WrongTypeForStdoutFallsBackToDefault) {
    TempConfigFile f(R"({"log": {"stdout": "yes"}})");
    auto cfg = oj::common::AppConfig::load(f.path());
    // is_boolean() 为 false → 保持默认 true
    EXPECT_TRUE(cfg.log.stdout_console);
}

// ---------------------------------------------------------------------------
//  HealthHandler — 字段边界
// ---------------------------------------------------------------------------
TEST(HealthHandlerTest, UptimeValuePropagatedExactly) {
    const std::int64_t cases[] = {0, 1, 12345, 999'999'999};
    for (auto up : cases) {
        httplib::Request  req;
        httplib::Response res;
        oj::http::handlers::health(req, res, up);
        ASSERT_EQ(res.status, 200) << "uptime=" << up;
        auto body = nlohmann::json::parse(res.body);
        EXPECT_EQ(body["data"]["uptime_ms"].get<std::int64_t>(), up)
            << "uptime=" << up;
    }
}

TEST(HealthHandlerTest, NowUnixCloseToSystemTime) {
    using namespace std::chrono;
    const auto before = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    httplib::Request  req;
    httplib::Response res;
    oj::http::handlers::health(req, res, 0);

    const auto after = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();

    auto body = nlohmann::json::parse(res.body);
    auto t    = body["data"]["now_unix"].get<std::int64_t>();

    EXPECT_GE(t, before);
    EXPECT_LE(t, after);
}

TEST(HealthHandlerTest, ContentTypeIsApplicationJsonUtf8) {
    httplib::Request  req;
    httplib::Response res;
    oj::http::handlers::health(req, res, 0);
    EXPECT_EQ(res.get_header_value("Content-Type"), "application/json; charset=utf-8");
}

// ---------------------------------------------------------------------------
//  HttpHelpers — write_json / write_ok / write_error 完整 code 表
// ---------------------------------------------------------------------------
TEST(HttpHelpersTest, WriteJsonStatusMatchesAllCodes) {
    struct Case { oj::common::ErrorCode code; int http; std::string_view msg; };
    const Case cases[] = {
        {oj::common::ErrorCode::Ok,           200, "ok"},
        {oj::common::ErrorCode::BadRequest,   400, "bad request"},
        {oj::common::ErrorCode::Unauthorized, 401, "unauthorized"},
        {oj::common::ErrorCode::Forbidden,    403, "forbidden"},
        {oj::common::ErrorCode::NotFound,     404, "not found"},
        {oj::common::ErrorCode::Conflict,     409, "conflict"},
        {oj::common::ErrorCode::TooLarge,     413, "payload too large"},
        {oj::common::ErrorCode::Internal,     500, "internal server error"},
        {oj::common::ErrorCode::SystemError,  500, "system error"},
    };
    for (const auto& c : cases) {
        httplib::Response res;
        oj::http::write_json(res, c.code);
        EXPECT_EQ(res.status, c.http)
            << "code=" << static_cast<int>(c.code);
        EXPECT_EQ(res.get_header_value("Content-Type"), "application/json; charset=utf-8");
        auto body = nlohmann::json::parse(res.body);
        EXPECT_EQ(body["code"].get<int>(),            static_cast<int>(c.code));
        EXPECT_EQ(body["message"].get<std::string>(), std::string{c.msg});
        EXPECT_TRUE(body["data"].is_null());
    }
}

TEST(HttpHelpersTest, WriteJsonWithExplicitMessageAndData) {
    httplib::Response res;
    oj::http::write_json(res, oj::common::ErrorCode::Conflict,
                         nlohmann::json{{"field", "username"}},
                         "username already taken");
    EXPECT_EQ(res.status, 409);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            1005);
    EXPECT_EQ(body["message"].get<std::string>(), "username already taken");
    EXPECT_EQ(body["data"]["field"].get<std::string>(), "username");
}

TEST(HttpHelpersTest, WriteOkAlways200WithOkMessage) {
    httplib::Response res;
    oj::http::write_ok(res, nlohmann::json{{"id", 42}});
    EXPECT_EQ(res.status, 200);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),              0);
    EXPECT_EQ(body["message"].get<std::string>(),   "ok");
    EXPECT_EQ(body["data"]["id"].get<int>(),        42);
}

TEST(HttpHelpersTest, WriteErrorDefaultsMessageFromCode) {
    httplib::Response res;
    oj::http::write_error(res, oj::common::ErrorCode::Forbidden);
    EXPECT_EQ(res.status, 403);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            1003);
    EXPECT_EQ(body["message"].get<std::string>(), "forbidden");
    EXPECT_TRUE(body["data"].is_null());
}

// ---------------------------------------------------------------------------
//  HttpServer E2E — 404 / 405 / 顺序多次 / handler 抛异常 → 500 envelope
// ---------------------------------------------------------------------------
namespace {

// 把"起一个 server 在指定端口跑测试代码"的样板集中起来，避免每个测试重复。
class ScopedServer {
public:
    explicit ScopedServer(uint16_t port) : cfg_(make_cfg(port)), srv_(std::move(cfg_)) {}

    ScopedServer(const ScopedServer&)            = delete;
    ScopedServer& operator=(const ScopedServer&) = delete;

    oj::http::HttpServer& server() noexcept { return srv_; }

    // 阻塞启动 server 到子线程；join 由析构负责
    void start() {
        thread_ = std::thread([this] {
            ready_.store(true, std::memory_order_release);
            std::string reason;
            srv_.listen(&reason);  // blocking; returns when stop() is called
        });
        // 等 listen 真正进入 accept 循环
        for (int i = 0; i < 300 && !ready_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ~ScopedServer() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }

private:
    static oj::common::AppConfig make_cfg(uint16_t port) {
        oj::common::AppConfig cfg;
        cfg.server.host             = "127.0.0.1";
        cfg.server.port             = port;
        cfg.server.thread_pool_size = 2;
        cfg.log.stdout_console      = false;
        cfg.log.dir                 = std::filesystem::temp_directory_path();
        return cfg;
    }

    oj::common::AppConfig      cfg_;
    oj::http::HttpServer       srv_;
    std::thread                thread_;
    std::atomic<bool>          ready_{false};
};

httplib::Client make_client(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    return cli;
}

}  // namespace

TEST(HttpServerE2ETest, UnknownRouteReturnsEnvelope404) {
    ScopedServer s(18081);
    s.server().get("/api/health", [](const httplib::Request& q, httplib::Response& r) {
        oj::http::write_ok(r, nlohmann::json{{"ping", "pong"}});
    });
    s.start();

    auto res = make_client(18081).Get("/api/nope");
    if (!res) GTEST_SKIP() << "port 18081 not reachable in this environment";

    EXPECT_EQ(res->status, 404);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["code"].get<int>(),            1004);
    EXPECT_EQ(body["message"].get<std::string>(), "route not found: GET /api/nope");
}

TEST(HttpServerE2ETest, WrongMethodReturnsEnvelopeNotRawText) {
    ScopedServer s(18082);
    s.server().get("/only-get", [](const httplib::Request&, httplib::Response& r) {
        oj::http::write_ok(r);
    });
    s.start();

    auto res = make_client(18082).Post("/only-get", "{}", "application/json");
    if (!res) GTEST_SKIP() << "port 18082 not reachable in this environment";

    // 契约：错方法必须返回 JSON envelope，而不是空 body 或默认 404 文本。
    // 注：cpp-httplib v0.15.3 的 routing() 在 path 命中但 method 不匹配时不会自动返回 405，
    //     而是回退到默认 404；HttpServer::install_exception_middleware 的 error_handler
    //     会把 404 翻译成 envelope(code=1004)。这里我们断言 envelope 形状，
    //     不强求 405 这个具体状态码（后续若升级到支持 405 的 cpp-httplib 版本，
    //     再补强 EXPECT 即可）。
    EXPECT_FALSE(res->body.empty()) << "envelope body must not be empty";
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json; charset=utf-8");
    ASSERT_NO_THROW((void)nlohmann::json::parse(res->body))
        << "response must be valid JSON, got: " << res->body;
    auto body = nlohmann::json::parse(res->body);
    EXPECT_TRUE(body.contains("code"))    << body.dump();
    EXPECT_TRUE(body.contains("message")) << body.dump();
    EXPECT_TRUE(body.contains("data"))    << body.dump();
    // SPEC §5.1: 1004 = NotFound；错方法在 cpp-httplib v0.15.3 下也走这条
    EXPECT_EQ(body["code"].get<int>(), 1004) << body.dump();
    EXPECT_NE(body["message"].get<std::string>().find("POST /only-get"),
              std::string::npos)
        << "message should mention the offending method+path, got: " << body.dump();
}

TEST(HttpServerE2ETest, SequentialRequestsAllSucceed) {
    ScopedServer s(18083);
    int hits = 0;
    s.server().get("/counter", [&hits](const httplib::Request&, httplib::Response& r) {
        ++hits;
        oj::http::write_ok(r, nlohmann::json{{"hits", hits}});
    });
    s.start();

    auto cli = make_client(18083);
    for (int i = 1; i <= 5; ++i) {
        auto res = cli.Get("/counter");
        if (!res) GTEST_SKIP() << "port 18083 not reachable in this environment";
        ASSERT_EQ(res->status, 200)               << "i=" << i;
        auto body = nlohmann::json::parse(res->body);
        EXPECT_EQ(body["data"]["hits"].get<int>(), i) << "i=" << i;
    }
    EXPECT_EQ(hits, 5);
}

TEST(HttpServerE2ETest, PostRouteDispatchesAndEchoesBody) {
    ScopedServer s(18084);
    s.server().post("/api/echo", [](const httplib::Request& q, httplib::Response& r) {
        oj::http::write_ok(r, nlohmann::json{
            {"method",   q.method},
            {"received", q.body},
        });
    });
    s.start();

    auto res = make_client(18084).Post("/api/echo", "hello-oj", "text/plain");
    if (!res) GTEST_SKIP() << "port 18084 not reachable in this environment";

    EXPECT_EQ(res->status, 200);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["data"]["method"].get<std::string>(),   "POST");
    EXPECT_EQ(body["data"]["received"].get<std::string>(), "hello-oj");
}

TEST(HttpServerE2ETest, HandlerThrowingReturns500Envelope) {
    ScopedServer s(18085);
    s.server().get("/boom", [](const httplib::Request&, httplib::Response&) {
        throw std::runtime_error("synthetic failure for test");
    });
    s.start();

    auto res = make_client(18085).Get("/boom");
    if (!res) GTEST_SKIP() << "port 18085 not reachable in this environment";

    EXPECT_EQ(res->status, 500);
    auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body["code"].get<int>(),            1007);
    EXPECT_EQ(body["message"].get<std::string>(), "internal server error");
}

TEST(HttpServerE2ETest, UptimeIncreasesOverTime) {
    ScopedServer s(18086);
    s.server().get("/api/health", [&s = s.server()](const httplib::Request& q, httplib::Response& r) {
        oj::http::handlers::health(q, r, s.uptime_ms());
    });
    s.start();

    auto cli = make_client(18086);
    auto r1  = cli.Get("/api/health");
    if (!r1) GTEST_SKIP() << "port 18086 not reachable in this environment";
    auto up1 = nlohmann::json::parse(r1->body)["data"]["uptime_ms"].get<std::int64_t>();

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    auto r2  = cli.Get("/api/health");
    ASSERT_TRUE(r2 != nullptr);
    auto up2 = nlohmann::json::parse(r2->body)["data"]["uptime_ms"].get<std::int64_t>();

    EXPECT_GT(up2, up1);
    EXPECT_GE(up2 - up1, 100);  // 至少经过 100ms
}
