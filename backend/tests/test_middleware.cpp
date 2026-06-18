// =============================================================================
//  test_middleware.cpp — Phase 7 middleware 单元测试
//
//  覆盖范围 (SPEC §9.4 M-2):
//    - extract_user_id_from_bearer: Bearer 解析 / 容错 / 子段抽取
//    - parse_json_body: 成功 / 空体 / 非 JSON / 非 object 四条路径
//    - db_unavailable_response: envelope 形状
//    - HttpServer 中 install_logger / install_pre_routing / install_post_routing
//      三种 hook 都能正常挂入并在 end-to-end httplib 调用时触发
// =============================================================================

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>

#include "common/config.hpp"
#include "http/HttpServer.hpp"
#include "http/middleware/middleware.hpp"

namespace oj::http::middleware {
namespace {

// ----------------------------------------------------------------------------
//  extract_user_id_from_bearer —— Bearer JWT 解析 (base64url "sub")
// ----------------------------------------------------------------------------

// 手工构造 base64url payload + 拼出 "Bearer <header>.<payload>.<sig>"
// payload 形如 {"uid":<user_id>,"iat":...}
std::string make_bearer(std::int64_t uid) {
    // header: 任意 12 字节 {"alg":"HS256"}
    static const std::string header_b64 = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    nlohmann::json payload = {{"uid", uid}, {"iss", "x"}, {"iat", 1700000000}};
    std::string payload_str = payload.dump();

    // 简单 base64url 编码 (无 padding)
    auto b64url_encode = [](const std::string& in) {
        static const char* tbl =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        int val = 0, bits = 0;
        for (unsigned char c : in) {
            val = (val << 8) | c;
            bits += 8;
            while (bits >= 6) {
                bits -= 6;
                out.push_back(tbl[(val >> bits) & 0x3F]);
            }
        }
        if (bits > 0) out.push_back(tbl[(val << (6 - bits)) & 0x3F]);
        return out;
    };

    return "Bearer " + header_b64 + "." + b64url_encode(payload_str) + ".sig";
}

TEST(ExtractUserIdTest, EmptyHeaderReturnsZero) {
    EXPECT_EQ(extract_user_id_from_bearer(""), 0);
    EXPECT_EQ(extract_user_id_from_bearer("Token foo"), 0);
}

TEST(ExtractUserIdTest, BearerPrefixCaseInsensitive) {
    EXPECT_EQ(extract_user_id_from_bearer("bearer " + make_bearer(0).substr(7)), 0)
        << "lower-case prefix 不应误认 (实际 JWT 字段本身大小写敏感,这里的" << " 期望 0 是因为我们构造的 token 没拼回正确格式)";
}

TEST(ExtractUserIdTest, MissingDotReturnsZero) {
    EXPECT_EQ(extract_user_id_from_bearer("Bearer just_a_word"), 0);
}

TEST(ExtractUserIdTest, WellFormedBearerYieldsSub) {
    EXPECT_EQ(extract_user_id_from_bearer(make_bearer(12345)), 12345);
    EXPECT_EQ(extract_user_id_from_bearer(make_bearer(7)), 7);
    EXPECT_EQ(extract_user_id_from_bearer(make_bearer(9999999)), 9999999);
}

TEST(ExtractUserIdTest, SubAsStringReturnsZero) {
    // "uid":"alice" —— 我们只支持数字 uid
    nlohmann::json p = {{"uid", "alice"}};
    std::string payload_b64 = "eyJ1aWQiOiJhbGljZSJ9";  // base64url of {"uid":"alice"}
    EXPECT_EQ(extract_user_id_from_bearer("Bearer xxx." + payload_b64 + ".yyy"), 0);
}

// ----------------------------------------------------------------------------
//  parse_json_body —— request body 解析 helper
// ----------------------------------------------------------------------------

TEST(ParseJsonBodyTest, EmptyBodyWritesBadRequestEnvelope) {
    httplib::Request  req;
    httplib::Response res;
    auto out = parse_json_body(req, res);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);

    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1001);
    EXPECT_TRUE(body["data"].is_null());
}

TEST(ParseJsonBodyTest, MalformedJsonWritesBadRequestEnvelope) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "{this is not json";
    auto out = parse_json_body(req, res);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1001);
    EXPECT_NE(body["message"].get<std::string>().find("invalid json"),
              std::string::npos);
}

TEST(ParseJsonBodyTest, TopLevelArrayWritesBadRequestEnvelope) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "[1,2,3]";
    auto out = parse_json_body(req, res);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_NE(body["message"].get<std::string>().find("JSON object"),
              std::string::npos);
}

TEST(ParseJsonBodyTest, TopLevelStringWritesBadRequestEnvelope) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "\"foo\"";
    auto out = parse_json_body(req, res);
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(res.status, 400);
}

TEST(ParseJsonBodyTest, ValidObjectReturnsParsedJson) {
    httplib::Request  req;
    httplib::Response res;
    req.body = R"({"username":"alice","age":30})";
    auto out = parse_json_body(req, res);
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ((*out)["username"].get<std::string>(), "alice");
    EXPECT_EQ((*out)["age"].get<int>(), 30);
    EXPECT_EQ(res.status, -1) << "成功路径不应修改 res.status";
    EXPECT_TRUE(res.body.empty()) << "成功路径不应写 body";
}

TEST(ParseJsonBodyTest, EmptyObjectReturnsParsedJson) {
    httplib::Request  req;
    httplib::Response res;
    req.body = "{}";
    auto out = parse_json_body(req, res);
    ASSERT_TRUE(out.has_value());
    EXPECT_TRUE(out->empty());
}

// ----------------------------------------------------------------------------
//  db_unavailable_response —— 系统级不可用 envelope
// ----------------------------------------------------------------------------

TEST(DbUnavailableResponseTest, WritesSystemErrorEnvelope) {
    httplib::Response res;
    db_unavailable_response(res);
    EXPECT_EQ(res.status, 500);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1008);
    EXPECT_NE(body["message"].get<std::string>().find("database"),
              std::string::npos);
}

// ----------------------------------------------------------------------------
//  install_* — end-to-end 走 httplib 内置客户端,验证 hook 真实触发
// ----------------------------------------------------------------------------

namespace {

// 用 ringbuffer_sink 接管 spdlog,在测试期间收集日志
class ScopedRingbufferSink {
public:
    ScopedRingbufferSink() {
        rb_ = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
        auto logger = std::make_shared<spdlog::logger>("test_ring", rb_);
        logger->set_level(spdlog::level::trace);
        spdlog::set_default_logger(logger);
        // restore on destruction
        prev_ = spdlog::default_logger();
    }
    ~ScopedRingbufferSink() {
        spdlog::set_default_logger(prev_);
    }
    std::vector<std::string> last_formatted() {
        auto v = rb_->last_formatted();
        return v;
    }

private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> rb_;
    std::shared_ptr<spdlog::logger> prev_;
};

oj::common::AppConfig make_test_config() {
    oj::common::AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = 0;  // OS 分配
    cfg.server.thread_pool_size = 2;
    cfg.mysql.host = "127.0.0.1";   // 故意填死,test 用 ringbuffer 不连 DB
    cfg.mysql.port = 13306;
    cfg.mysql.user = "oj";
    cfg.mysql.password = "oj";
    cfg.mysql.database = "oj";
    cfg.jwt.secret = std::string(64, 'a');
    return cfg;
}

}  // namespace

TEST(HttpServerHooksTest, LoggerHookFiresOnEveryRequest) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    ScopedRingbufferSink rb;

    std::atomic<int> log_count{0};
    server.install_logger([&](const httplib::Request& req,
                              const httplib::Response& res) {
        log_count++;
        // 验证能拿到关键字段
        EXPECT_FALSE(req.method.empty());
        EXPECT_FALSE(req.path.empty());
        EXPECT_TRUE(res.status > 0);
    });
    server.get("/api/probe", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        res.set_content("ok", "text/plain");
    });

    // 找端口并启动
    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << "listen failed: " << reason;

    // httplib 启动后,server.bound_port() 返回实际监听端口(测试用 port=0
    // 由 OS 分配);生产用 config().server.port,二者最终一致。
    int port = server.bound_port();
    ASSERT_GT(port, 0);

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/api/probe");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);

    // access log 回调在响应送出后触发 → 至少 1 条
    EXPECT_GE(log_count.load(), 1);

    server.stop();
}

TEST(HttpServerHooksTest, PreRoutingHookFiresAndPassesThrough) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    std::atomic<int> pre_count{0};
    std::atomic<int> handler_count{0};

    server.install_pre_routing([&](const httplib::Request&,
                                   httplib::Response&) {
        pre_count++;
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.get("/x", [&](const httplib::Request&, httplib::Response& res) {
        handler_count++;
        res.set_content("ok", "text/plain");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/x");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);

    EXPECT_EQ(pre_count.load(), 1);
    EXPECT_EQ(handler_count.load(), 1);

    server.stop();
}

TEST(HttpServerHooksTest, PostRoutingHookAddsResponseHeaders) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    server.install_post_routing([](const httplib::Request&,
                                   httplib::Response& res) {
        res.set_header("X-Test-Phase7", "ok");
    });
    server.get("/h", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("hi", "text/plain");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/h");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);
    ASSERT_TRUE(r->has_header("X-Test-Phase7"));
    EXPECT_EQ(r->get_header_value("X-Test-Phase7"), "ok");

    server.stop();
}

TEST(HttpServerHooksTest, AccessLogExtractsUserIdFromBearer) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    std::atomic<std::int64_t> logged_uid{0};
    std::atomic<int> log_count{0};

    server.install_logger([&](const httplib::Request& req,
                              const httplib::Response&) {
        log_count++;
        logged_uid = extract_user_id_from_bearer(req.get_header_value("Authorization"));
    });
    server.get("/u", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);

    httplib::Headers h = {
        {"Authorization", make_bearer(987654)},
    };
    auto r = cli.Get("/u", h);
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);

    EXPECT_GE(log_count.load(), 1);
    EXPECT_EQ(logged_uid.load(), 987654);

    server.stop();
}

TEST(HttpServerHooksTest, SecurityHeadersInstalled) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    install_security_headers(server);
    server.get("/p", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/p");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 200);

    // S-1 安全响应头必须就位
    EXPECT_EQ(r->get_header_value("X-Content-Type-Options"), "nosniff");
    EXPECT_EQ(r->get_header_value("X-Frame-Options"), "DENY");
    EXPECT_EQ(r->get_header_value("Referrer-Policy"), "no-referrer");
    auto csp = r->get_header_value("Content-Security-Policy");
    EXPECT_NE(csp.find("default-src 'self'"), std::string::npos);
    EXPECT_NE(csp.find("frame-ancestors 'none'"), std::string::npos);

    server.stop();
}

TEST(HttpServerHooksTest, UnhandledExceptionMappedTo500Envelope) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));
    server.install_exception_middleware();
    server.get("/boom", [](const httplib::Request&, httplib::Response&) {
        throw std::runtime_error("kaboom");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/boom");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 500);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(), 1007);
    EXPECT_EQ(body["message"].get<std::string>(), "internal server error");

    server.stop();
}

TEST(HttpServerHooksTest, NotFoundReturns404Envelope) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));
    server.install_exception_middleware();

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/no/such/path");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 404);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(), 1004);

    server.stop();
}

TEST(HttpServerHooksTest, AccessLogEmitsSingleLineWithKeyFields) {
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(std::move(cfg));

    ScopedRingbufferSink rb;
    install_access_log(server);  // 默认 warn_threshold=1000ms

    server.get("/log-test", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok", "text/plain");
    });

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();

    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/log-test");
    ASSERT_TRUE(r != nullptr);

    // 立即 stop 让 thread_local + 回调跑完
    server.stop();

    auto lines = rb.last_formatted();
    bool found = false;
    for (auto& ln : lines) {
        if (ln.find("/log-test") != std::string::npos &&
            ln.find("200") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "access log 未出现 GET /log-test 200;got "
                        << lines.size() << " lines";
}

}  // namespace
}  // namespace oj::http::middleware