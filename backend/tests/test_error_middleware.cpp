// =============================================================================
//  test_error_middleware.cpp — 统一错误中间件 (Phase 7.2) 单元测试
//
//  覆盖范围 (SPEC §9.4 M-2 + §5.1):
//    1. HttpError — 构造 / 工厂方法 / what() / code() / 类型派生关系
//    2. wrap_handler — 透传成功 / HttpError → envelope / std::exception → 1007
//                       / 未知异常 → 1007 / 多次连续调用
//    3. check_db_ready — true / false / is_db_ready 为空 三条路径
//    4. parse_path_id — 合法 / 缺 / 非整数 / 0 / 负数 / 前后空白
//    5. parse_query_int — 合法 / 缺 / 空 / 非整数 / 越界 / 范围配置
//    6. require_string_field — 缺 / null / 非 string / 合法 / 中文
//    7. E2E (走 httplib 客户端) — handler 抛 HttpError / std::exception 的
//       end-to-end envelope 形状 + Content-Type + status code
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include "common/error_code.hpp"
#include "http/HttpServer.hpp"
#include "http/middleware/error.hpp"

namespace oj::http::middleware {
namespace {

// -----------------------------------------------------------------------------
//  HttpError
// -----------------------------------------------------------------------------

TEST(HttpErrorTest, ConstructorStoresCodeAndMessage) {
    HttpError e(oj::common::ErrorCode::BadRequest, "username too short");
    EXPECT_EQ(e.code(), oj::common::ErrorCode::BadRequest);
    EXPECT_STREQ(e.what(), "username too short");
}

TEST(HttpErrorTest, InheritsFromStdRuntimeError) {
    // 必须能被 catch (const std::exception&) catch,这是 wrap_handler 的前提
    HttpError e(oj::common::ErrorCode::NotFound, "missing");
    try {
        throw e;
    } catch (const std::exception& base) {
        EXPECT_STREQ(base.what(), "missing");
    }
}

TEST(HttpErrorTest, EmptyMessageFallsBackToCodeString) {
    HttpError e(oj::common::ErrorCode::Forbidden, "");
    EXPECT_EQ(e.code(), oj::common::ErrorCode::Forbidden);
    EXPECT_STREQ(e.what(), "forbidden")
        << "空 message 应回退为 to_string(code)";
}

TEST(HttpErrorTest, FactoryMethodsCoverAllErrorCodes) {
    using oj::common::ErrorCode;
    EXPECT_EQ(HttpError::bad_request("x").code(),   ErrorCode::BadRequest);
    EXPECT_EQ(HttpError::unauthorized("x").code(), ErrorCode::Unauthorized);
    EXPECT_EQ(HttpError::forbidden("x").code(),    ErrorCode::Forbidden);
    EXPECT_EQ(HttpError::not_found("x").code(),     ErrorCode::NotFound);
    EXPECT_EQ(HttpError::conflict("x").code(),     ErrorCode::Conflict);
    EXPECT_EQ(HttpError::too_large("x").code(),     ErrorCode::TooLarge);
    EXPECT_EQ(HttpError::internal("x").code(),     ErrorCode::Internal);
    EXPECT_EQ(HttpError::system_error("x").code(), ErrorCode::SystemError);
}

TEST(HttpErrorTest, FactoryMethodsDefaultMessage) {
    EXPECT_STREQ(HttpError::bad_request().what(),     "bad request");
    EXPECT_STREQ(HttpError::unauthorized().what(),   "unauthorized");
    EXPECT_STREQ(HttpError::forbidden().what(),       "forbidden");
    EXPECT_STREQ(HttpError::not_found().what(),       "not found");
    EXPECT_STREQ(HttpError::too_large().what(),       "payload too large");
    EXPECT_STREQ(HttpError::internal().what(),        "internal server error");
    EXPECT_STREQ(HttpError::system_error().what(),    "system error");
}

TEST(HttpErrorTest, NonAsciiMessagePreserved) {
    HttpError e(oj::common::ErrorCode::BadRequest, "用户名长度必须在 3–20 字符之间");
    EXPECT_STREQ(e.what(), "用户名长度必须在 3–20 字符之间");
}

// -----------------------------------------------------------------------------
//  check_db_ready
// -----------------------------------------------------------------------------

TEST(CheckDbReadyTest, ReadyReturnsTrueAndWritesNothing) {
    httplib::Response res;
    auto is_ready = []() { return true; };
    EXPECT_TRUE(check_db_ready(res, is_ready));
    EXPECT_TRUE(res.body.empty()) << "ready 时不应写 body";
    EXPECT_EQ(res.status, -1)      << "ready 时不应动 status";
}

TEST(CheckDbReadyTest, NotReadyWrites1008Envelope) {
    httplib::Response res;
    auto is_ready = []() { return false; };
    EXPECT_FALSE(check_db_ready(res, is_ready));

    EXPECT_EQ(res.status, 500);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1008);
    EXPECT_NE(body["message"].get<std::string>().find("database"),
              std::string::npos);
}

TEST(CheckDbReadyTest, NullCallbackTreatedAsReady) {
    httplib::Response res;
    // 即便 DB readiness 回调为空 (如 main.cpp 没接 mysql) ,也视作 ready
    std::function<bool()> none;
    EXPECT_TRUE(check_db_ready(res, none));
    EXPECT_TRUE(res.body.empty());
}

// -----------------------------------------------------------------------------
//  parse_path_id
// -----------------------------------------------------------------------------

TEST(ParsePathIdTest, ReturnsValueForValidId) {
    httplib::Request req;
    req.path_params["id"] = "42";
    auto v = parse_path_id(req, "id");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, 42);
}

TEST(ParsePathIdTest, MissingReturnsNullopt) {
    httplib::Request req;
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value());
}

TEST(ParsePathIdTest, NonNumericReturnsNullopt) {
    httplib::Request req;
    req.path_params["id"] = "abc";
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value());
}

TEST(ParsePathIdTest, MixedNumericReturnsNullopt) {
    httplib::Request req;
    req.path_params["id"] = "42abc";
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value()) << "部分数字仍应失败(整段匹配)";
}

TEST(ParsePathIdTest, ZeroReturnsNullopt) {
    httplib::Request req;
    req.path_params["id"] = "0";
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value()) << "id 必须 > 0";
}

TEST(ParsePathIdTest, NegativeReturnsNullopt) {
    httplib::Request req;
    req.path_params["id"] = "-5";
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value());
}

TEST(ParsePathIdTest, EmptyStringReturnsNullopt) {
    httplib::Request req;
    req.path_params["id"] = "";
    auto v = parse_path_id(req, "id");
    EXPECT_FALSE(v.has_value());
}

TEST(ParsePathIdTest, OtherNamePreserved) {
    httplib::Request req;
    req.path_params["user_id"] = "7";
    EXPECT_EQ(parse_path_id(req, "user_id"), 7);
    EXPECT_FALSE(parse_path_id(req, "submission_id").has_value());
}

// -----------------------------------------------------------------------------
//  parse_query_int
// -----------------------------------------------------------------------------

TEST(ParseQueryIntTest, ValidValueParsed) {
    httplib::Request req;
    req.params.insert({"page", "3"});
    EXPECT_EQ(parse_query_int(req, "page"), 3);
}

TEST(ParseQueryIntTest, MissingReturnsNullopt) {
    httplib::Request req;
    EXPECT_FALSE(parse_query_int(req, "page").has_value());
}

TEST(ParseQueryIntTest, EmptyValueReturnsNullopt) {
    httplib::Request req;
    req.params.insert({"page", ""});
    EXPECT_FALSE(parse_query_int(req, "page").has_value());
}

TEST(ParseQueryIntTest, NonNumericReturnsNullopt) {
    httplib::Request req;
    req.params.insert({"page", "abc"});
    EXPECT_FALSE(parse_query_int(req, "page").has_value());
}

TEST(ParseQueryIntTest, MinBoundEnforced) {
    httplib::Request req;
    req.params.insert({"page", "0"});
    QueryIntOptions opts;
    opts.min_value = 1;
    EXPECT_FALSE(parse_query_int(req, "page", opts).has_value());

    // httplib::Params 是 multimap,insert 不替换;先清空再插入新值
    req.params.erase("page");
    req.params.insert({"page", "1"});
    EXPECT_EQ(parse_query_int(req, "page", opts), 1);
}

TEST(ParseQueryIntTest, MaxBoundEnforced) {
    httplib::Request req;
    req.params.insert({"size", "1000"});
    QueryIntOptions opts;
    opts.max_value = 100;
    EXPECT_FALSE(parse_query_int(req, "size", opts).has_value());

    req.params.erase("size");
    req.params.insert({"size", "100"});
    EXPECT_EQ(parse_query_int(req, "size", opts), 100);
}

TEST(ParseQueryIntTest, ZeroIsValidUnlessMinSet) {
    httplib::Request req;
    req.params.insert({"size", "0"});
    // 不设 min → 0 合法
    EXPECT_EQ(parse_query_int(req, "size"), 0);
    // 设 min=1 → 0 非法
    QueryIntOptions opts;
    opts.min_value = 1;
    EXPECT_FALSE(parse_query_int(req, "size", opts).has_value());
}

// -----------------------------------------------------------------------------
//  require_string_field
// -----------------------------------------------------------------------------

TEST(RequireStringFieldTest, ReturnsValue) {
    nlohmann::json body = {{"username", "alice"}};
    EXPECT_EQ(require_string_field(body, "username"), "alice");
}

TEST(RequireStringFieldTest, MissingThrowsBadRequest) {
    nlohmann::json body = {{"email", "a@b.c"}};
    try {
        require_string_field(body, "username");
        FAIL() << "expected HttpError";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.code(), oj::common::ErrorCode::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("username"), std::string::npos);
    }
}

TEST(RequireStringFieldTest, NullThrowsBadRequest) {
    nlohmann::json body = {{"username", nullptr}};
    EXPECT_THROW(require_string_field(body, "username"), HttpError);
}

TEST(RequireStringFieldTest, NonStringTypeThrowsBadRequest) {
    nlohmann::json body = {{"username", 12345}};
    try {
        require_string_field(body, "username");
        FAIL() << "expected HttpError";
    } catch (const HttpError& e) {
        EXPECT_EQ(e.code(), oj::common::ErrorCode::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("string"), std::string::npos);
    }
}

TEST(RequireStringFieldTest, NonAsciiValuePreserved) {
    nlohmann::json body = {{"username", "张三"}};
    EXPECT_EQ(require_string_field(body, "username"), "张三");
}

TEST(RequireStringFieldTest, EmptyStringIsValid) {
    nlohmann::json body = {{"username", ""}};
    // require_string_field 不校验非空(那是 domain 层的活);空串合法
    EXPECT_EQ(require_string_field(body, "username"), "");
}

// -----------------------------------------------------------------------------
//  wrap_handler —— 单元级 (直接调用,不走 httplib)
// -----------------------------------------------------------------------------

namespace {

// 在单元级 wrap_handler 测试中,把每次 handler 调用结束时的 res 状态保存下来
struct CapturedResponse {
    int         status{-1};
    std::string body;
    std::string content_type;
};

}  // namespace

TEST(WrapHandlerTest, SuccessPathPassesThrough) {
    auto inner = [](const httplib::Request&, httplib::Response& res) {
        oj::http::write_ok(res, nlohmann::json{{"hello", "world"}});
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_EQ(res.status, 200);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            0);
    EXPECT_EQ(body["data"]["hello"].get<std::string>(), "world");
}

TEST(WrapHandlerTest, HttpErrorTranslatesToEnvelope) {
    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw HttpError::bad_request("username too short");
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_EQ(res.status, 400);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            1001);
    EXPECT_EQ(body["message"].get<std::string>(), "username too short");
}

TEST(WrapHandlerTest, HttpErrorPreservesAllCodes) {
    struct Case { oj::common::ErrorCode code; int http; };
    const Case cases[] = {
        {oj::common::ErrorCode::BadRequest,   400},
        {oj::common::ErrorCode::Unauthorized, 401},
        {oj::common::ErrorCode::Forbidden,    403},
        {oj::common::ErrorCode::NotFound,     404},
        {oj::common::ErrorCode::Conflict,     409},
        {oj::common::ErrorCode::TooLarge,     413},
        {oj::common::ErrorCode::Internal,     500},
        {oj::common::ErrorCode::SystemError,  500},
    };
    for (const auto& c : cases) {
        // 注意:cases 数组里 Internal / SystemError 都走 500,但 code 不同
        auto inner = [c](const httplib::Request&, httplib::Response&) {
            throw HttpError(c.code, "msg");
        };
        auto wrapped = wrap_handler(inner);

        httplib::Request  req;
        httplib::Response res;
        wrapped(req, res);

        EXPECT_EQ(res.status, c.http) << "code=" << static_cast<int>(c.code);
        auto body = nlohmann::json::parse(res.body);
        EXPECT_EQ(body["code"].get<int>(), static_cast<int>(c.code))
            << "code=" << static_cast<int>(c.code);
        EXPECT_EQ(body["message"].get<std::string>(), "msg");
    }
}

TEST(WrapHandlerTest, StdExceptionBecomesInternalError) {
    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw std::runtime_error("unexpected database error");
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_EQ(res.status, 500);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(),            1007);
    EXPECT_EQ(body["message"].get<std::string>(), "internal server error")
        << "内部异常细节不应外泄";
}

TEST(WrapHandlerTest, NonStdExceptionBecomesInternalError) {
    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw 42;  // 抛 int —— 非 std 异常
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_EQ(res.status, 500);
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1007);
}

TEST(WrapHandlerTest, OverwritesPartialBody) {
    // 业务代码半路 set_content 后又抛 → wrap_handler 仍写 envelope
    auto inner = [](const httplib::Request&, httplib::Response& res) {
        res.set_content("partial", "text/plain");
        throw HttpError::not_found("resource gone");
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    EXPECT_EQ(res.status, 404);
    EXPECT_EQ(res.get_header_value("Content-Type"), "application/json; charset=utf-8");
    auto body = nlohmann::json::parse(res.body);
    EXPECT_EQ(body["code"].get<int>(), 1004);
}

TEST(WrapHandlerTest, MultipleCallsIndependent) {
    // 第一次抛,第二次成功 → 验证 handler 异常没有污染闭包状态
    std::atomic<int> calls{0};
    auto inner = [&calls](const httplib::Request&, httplib::Response& res) {
        if (calls.fetch_add(1) == 0) {
            throw HttpError::conflict("first call conflict");
        }
        oj::http::write_ok(res, nlohmann::json{{"call", calls.load()}});
    };
    auto wrapped = wrap_handler(inner);

    {
        httplib::Request  req;
        httplib::Response res;
        wrapped(req, res);
        EXPECT_EQ(res.status, 409);
    }
    {
        httplib::Request  req;
        httplib::Response res;
        wrapped(req, res);
        EXPECT_EQ(res.status, 200);
        auto body = nlohmann::json::parse(res.body);
        EXPECT_EQ(body["data"]["call"].get<int>(), 2);
    }
}

// -----------------------------------------------------------------------------
//  wrap_handler —— 集成级 (走真实 httplib 客户端,模拟生产路径)
// -----------------------------------------------------------------------------

namespace {

oj::common::AppConfig make_test_config() {
    oj::common::AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = 0;  // OS 分配
    cfg.server.thread_pool_size = 2;
    cfg.log.stdout_console = false;
    cfg.log.dir = std::filesystem::temp_directory_path();
    cfg.jwt.secret = std::string(64, 'a');
    return cfg;
}

// 起一个 server 在 OS 分配的端口,跑 wrapped 路由,返回 port。
// RAII: 析构时自动 stop server。
class ScopedWrappedServer {
public:
    explicit ScopedWrappedServer(Handler wrapped_handler) {
        cfg_ = make_test_config();
        server_ = std::make_unique<oj::http::HttpServer>(cfg_);
        // 同时挂 GET / POST,让单测既能测 GET 也能测 POST 流程
        server_->get ("/probe", wrapped_handler);
        server_->post("/probe", wrapped_handler);
        std::string reason;
        if (!server_->start_async(&reason)) {
            throw std::runtime_error("server start failed: " + reason);
        }
        port_ = server_->bound_port();
    }
    ~ScopedWrappedServer() {
        if (server_) server_->stop();
    }
    int port() const noexcept { return port_; }
private:
    oj::common::AppConfig            cfg_{};
    std::unique_ptr<oj::http::HttpServer> server_;
    int port_{0};
};

}  // namespace

TEST(WrapHandlerE2ETest, HttpErrorPropagatesOverHttp) {
    ScopedWrappedServer s(wrap_handler(
        [](const httplib::Request&, httplib::Response&) {
            throw HttpError::unauthorized("missing or invalid access token");
        }));

    httplib::Client cli("127.0.0.1", s.port());
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/probe");
    ASSERT_TRUE(r != nullptr);

    EXPECT_EQ(r->status, 401);
    EXPECT_EQ(r->get_header_value("Content-Type"), "application/json; charset=utf-8");
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(),            1002);
    EXPECT_EQ(body["message"].get<std::string>(), "missing or invalid access token");
}

TEST(WrapHandlerE2ETest, StdExceptionMappedToInternalOverHttp) {
    ScopedWrappedServer s(wrap_handler(
        [](const httplib::Request&, httplib::Response&) {
            throw std::runtime_error("synthetic db failure");
        }));

    httplib::Client cli("127.0.0.1", s.port());
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/probe");
    ASSERT_TRUE(r != nullptr);

    EXPECT_EQ(r->status, 500);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(),            1007);
    EXPECT_EQ(body["message"].get<std::string>(), "internal server error")
        << "内部细节不应通过 500 响应泄漏";
}

TEST(WrapHandlerE2ETest, SuccessOverHttp) {
    ScopedWrappedServer s(wrap_handler(
        [](const httplib::Request&, httplib::Response& res) {
            oj::http::write_ok(res, nlohmann::json{{"ok", true}});
        }));

    httplib::Client cli("127.0.0.1", s.port());
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/probe");
    ASSERT_TRUE(r != nullptr);

    EXPECT_EQ(r->status, 200);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(), 0);
    EXPECT_EQ(body["data"]["ok"].get<bool>(), true);
}

// -----------------------------------------------------------------------------
//  E2E: 模拟 register 路由的 HttpError 风格 (sanity check 端到端一致性)
// -----------------------------------------------------------------------------

TEST(WrapHandlerE2ETest, RequireStringFieldFlowOverHttp) {
    ScopedWrappedServer s(wrap_handler(
        [](const httplib::Request& req, httplib::Response& res) {
            if (req.body.empty()) throw HttpError::bad_request("empty body");
            auto body = nlohmann::json::parse(req.body);
            if (!body.is_object())
                throw HttpError::bad_request("body must be JSON object");
            const std::string username = require_string_field(body, "username");
            oj::http::write_ok(res, nlohmann::json{{"username", username}});
        }));

    httplib::Client cli("127.0.0.1", s.port());
    cli.set_connection_timeout(2, 0);

    // 缺 username
    {
        auto r = cli.Post("/probe", R"({"email":"a@b.c"})", "application/json");
        ASSERT_TRUE(r != nullptr);
        EXPECT_EQ(r->status, 400);
        auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["code"].get<int>(), 1001);
        EXPECT_NE(body["message"].get<std::string>().find("username"),
                  std::string::npos);
    }

    // 合法
    {
        auto r = cli.Post("/probe", R"({"username":"alice"})", "application/json");
        ASSERT_TRUE(r != nullptr);
        EXPECT_EQ(r->status, 200);
        auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["data"]["username"].get<std::string>(), "alice");
    }
}

TEST(WrapHandlerE2ETest, CheckDbReadyAndPathIdFlowOverHttp) {
    // 把 check_db_ready + parse_path_id 也走一遍真实 HTTP 路径
    std::function<bool()> is_db_ready = []() { return true; };

    ScopedWrappedServer s(wrap_handler(
        [&is_db_ready](const httplib::Request& req, httplib::Response& res) {
            if (!check_db_ready(res, is_db_ready)) return;
            auto id = parse_path_id(req, "id");
            if (!id) throw HttpError::bad_request("id must be a positive integer");
            oj::http::write_ok(res, nlohmann::json{{"id", *id}});
        }));

    // 注意:parse_path_id 依赖 cpp-httplib 把 :id 注入到 path_params;
    // 这里 ScopedWrappedServer 注册的路由是 /probe(无 :id),所以要换种方式
    // 直接构造一个 server 用真实路径参数的方式跑
    (void)s;

    // 单独起一个用 :id 路径的 server
    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(cfg);
    server.get("/items/:id", wrap_handler(
        [&is_db_ready](const httplib::Request& req, httplib::Response& res) {
            if (!check_db_ready(res, is_db_ready)) return;
            auto id = parse_path_id(req, "id");
            if (!id) throw HttpError::bad_request("id must be a positive integer");
            oj::http::write_ok(res, nlohmann::json{{"id", *id}});
        }));

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;
    int port = server.bound_port();
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);

    {
        auto r = cli.Get("/items/123");
        ASSERT_TRUE(r != nullptr);
        EXPECT_EQ(r->status, 200);
        auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["data"]["id"].get<int>(), 123);
    }
    {
        auto r = cli.Get("/items/abc");
        ASSERT_TRUE(r != nullptr);
        EXPECT_EQ(r->status, 400);
        auto body = nlohmann::json::parse(r->body);
        EXPECT_EQ(body["code"].get<int>(), 1001);
    }

    server.stop();
}

TEST(WrapHandlerE2ETest, DbDownReturns1008OverHttp) {
    std::function<bool()> is_db_ready = []() { return false; };

    oj::common::AppConfig cfg = make_test_config();
    oj::http::HttpServer server(cfg);
    server.get("/api/x", wrap_handler(
        [&is_db_ready](const httplib::Request&, httplib::Response& res) {
            if (!check_db_ready(res, is_db_ready)) return;
            oj::http::write_ok(res);
        }));

    std::string reason;
    ASSERT_TRUE(server.start_async(&reason)) << reason;

    httplib::Client cli("127.0.0.1", server.bound_port());
    cli.set_connection_timeout(2, 0);
    auto r = cli.Get("/api/x");
    ASSERT_TRUE(r != nullptr);
    EXPECT_EQ(r->status, 500);
    auto body = nlohmann::json::parse(r->body);
    EXPECT_EQ(body["code"].get<int>(), 1008);

    server.stop();
}

// -----------------------------------------------------------------------------
//  Logging 行为 —— HttpError 走 warn,std::exception 走 error
//
//  实现策略:复用 test_middleware.cpp 的 ScopedRingbufferSink 模式 (ringbuffer
//  是 spdlog 官方支持的 sink,destruction 时不会污染全局状态),从格式化的日志
//  行里解析 [level] tag 判断 level。零自定义 sink,完全用 spdlog 已有的工具。
// -----------------------------------------------------------------------------

namespace {

// 与 test_middleware.cpp 相同的 ScopedRingbufferSink (局部复制,不跨 TU)
// 作用:把 spdlog 默认 logger 临时换成一个 ringbuffer,测试期间收集日志
class ScopedRingbufferSink {
public:
    ScopedRingbufferSink() {
        rb_ = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(128);
        auto logger = std::make_shared<spdlog::logger>("test_error_mw", rb_);
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::trace);
        spdlog::set_default_logger(logger);
        prev_ = spdlog::default_logger();
    }
    ~ScopedRingbufferSink() {
        spdlog::set_default_logger(prev_);
    }
    std::vector<std::string> last_formatted() { return rb_->last_formatted(); }

private:
    std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> rb_;
    std::shared_ptr<spdlog::logger>                   prev_;
};

// 从 spdlog 默认 pattern "[...][level][thread] payload" 提取 level
// 例: "[2026-06-19 20:30:22.587] [warning] [14] HttpError in handler ..."
//      → spdlog::level::warn
spdlog::level::level_enum parse_level_token(const std::string& line) {
    // 简单扫一遍找 "[warning]"/"[error]"/"[info]"/"["debug]" 等
    struct Pair { std::string_view token; spdlog::level::level_enum lvl; };
    static const Pair table[] = {
        {"[trace] ",   spdlog::level::trace},
        {"[debug] ",   spdlog::level::debug},
        {"[info] ",    spdlog::level::info},
        {"[warning] ", spdlog::level::warn},
        {"[error] ",   spdlog::level::err},
        {"[critical] ", spdlog::level::critical},
    };
    for (const auto& p : table) {
        if (line.find(p.token) != std::string::npos) return p.lvl;
    }
    return spdlog::level::off;  // 找不到 = 默认 off (即不应出现)
}

}  // namespace

TEST(WrapHandlerLoggingTest, HttpErrorLoggedAtWarnLevel) {
    ScopedRingbufferSink sink;

    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw HttpError::bad_request("username too short");
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    auto lines = sink.last_formatted();
    bool found_warn_with_text = false;
    for (const auto& ln : lines) {
        if (parse_level_token(ln) == spdlog::level::warn &&
            ln.find("HttpError") != std::string::npos &&
            ln.find("username too short") != std::string::npos) {
            found_warn_with_text = true;
            break;
        }
    }
    EXPECT_TRUE(found_warn_with_text)
        << "HttpError 应记 warn 日志(含 code + msg);实际 lines:\n"
        << [&]{
            std::string s;
            for (const auto& l : lines) s += "  " + l + "\n";
            return s;
        }();
}

TEST(WrapHandlerLoggingTest, StdExceptionLoggedAtErrorLevel) {
    ScopedRingbufferSink sink;

    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw std::runtime_error("boom from synthetic");
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    auto lines = sink.last_formatted();
    bool found_err_with_text = false;
    for (const auto& ln : lines) {
        if (parse_level_token(ln) == spdlog::level::err &&
            ln.find("unhandled std::exception") != std::string::npos &&
            ln.find("boom from synthetic") != std::string::npos) {
            found_err_with_text = true;
            break;
        }
    }
    EXPECT_TRUE(found_err_with_text)
        << "std::exception 应记 error 日志(含原始 e.what())";
}

TEST(WrapHandlerLoggingTest, NonStdExceptionLoggedAtErrorLevel) {
    ScopedRingbufferSink sink;

    auto inner = [](const httplib::Request&, httplib::Response&) {
        throw 42;  // 非 std 异常
    };
    auto wrapped = wrap_handler(inner);

    httplib::Request  req;
    httplib::Response res;
    wrapped(req, res);

    auto lines = sink.last_formatted();
    bool found = false;
    for (const auto& ln : lines) {
        if (parse_level_token(ln) == spdlog::level::err &&
            ln.find("non-std exception") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "未知异常应记 error 日志";
}

}  // namespace
}  // namespace oj::http::middleware
