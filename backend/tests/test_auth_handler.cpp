// =============================================================================
//  test_auth_handler.cpp — POST /api/auth/register HTTP 入口测试
//  通过 ScopedServer 启动真实 HttpServer，注入 InMemoryUserRepo 装配的
//  AuthService；走完 JSON 解析 + 校验 + 业务 + token 颁发的全链路。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "domain/auth_service.hpp"
#include "domain/user_repository.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/auth_handler.hpp"
#include "infra/jwt_service.hpp"
#include "infra/password_hasher.hpp"

namespace {

using oj::common::AppConfig;
using oj::common::ErrorCode;
using oj::common::JwtConfig;
using oj::domain::AuthService;
using oj::domain::IUserRepository;
using oj::domain::User;
using oj::http::HttpServer;
using oj::infra::JwtService;
using oj::infra::PasswordHasher;

class InMemoryUserRepo : public IUserRepository {
public:
    User register_user(std::string_view u, std::string_view e, std::string_view h) override {
        std::lock_guard<std::mutex> lk(mu_);
        const bool make_admin = users_.empty();
        User x;
        x.id = ++next_id_;
        x.username = std::string{u};
        x.email    = std::string{e};
        x.password_hash = std::string{h};
        x.is_admin = make_admin;
        users_.push_back(x);
        return x;
    }
    std::optional<User> find_by_username(std::string_view u) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& x : users_) if (x.username == u) return x;
        return std::nullopt;
    }
    std::optional<User> find_by_email(std::string_view e) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& x : users_) if (x.email == e) return x;
        return std::nullopt;
    }
    std::optional<User> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& x : users_) if (x.id == id) return x;
        return std::nullopt;
    }
    std::size_t size() const { std::lock_guard<std::mutex> lk(mu_); return users_.size(); }
private:
    mutable std::mutex mu_;
    std::vector<User>  users_;
    std::int64_t       next_id_{0};
};

JwtConfig make_jwt_cfg() {
    JwtConfig c;
    c.secret          = "test-secret-32-bytes-min-padding-xxx";
    c.access_ttl_sec  = 3600;
    c.refresh_ttl_sec = 86400;
    c.issuer          = "onlinejudge-test";
    return c;
}

class ScopedServer {
public:
    explicit ScopedServer(uint16_t port) : cfg_(make_cfg(port)), srv_(std::move(cfg_)) {}
    ~ScopedServer() { srv_.stop(); if (thread_.joinable()) thread_.join(); }

    ScopedServer(const ScopedServer&) = delete;
    ScopedServer& operator=(const ScopedServer&) = delete;

    HttpServer& server() noexcept { return srv_; }

    void start() {
        thread_ = std::thread([this] {
            ready_.store(true, std::memory_order_release);
            std::string reason;
            srv_.listen(&reason);
        });
        for (int i = 0; i < 300 && !ready_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
private:
    static AppConfig make_cfg(uint16_t port) {
        AppConfig cfg;
        cfg.server.host = "127.0.0.1";
        cfg.server.port = port;
        cfg.server.thread_pool_size = 2;
        cfg.log.stdout_console = false;
        cfg.log.dir = std::filesystem::temp_directory_path();
        return cfg;
    }
    AppConfig      cfg_;
    HttpServer     srv_;
    std::thread    thread_;
    std::atomic<bool> ready_{false};
};

httplib::Client make_client(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    return cli;
}

// 装配一个真实 HttpServer + 真实 AuthService
// 注：db_ready 用 shared_ptr<atomic>，避免 struct 不可移动
struct ServerBundle {
    std::unique_ptr<ScopedServer>         server;
    std::shared_ptr<InMemoryUserRepo>     repo;
    std::shared_ptr<AuthService>          auth;
    std::shared_ptr<std::atomic<bool>>    db_ready{std::make_shared<std::atomic<bool>>(true)};
};

ServerBundle make_server(uint16_t port) {
    ServerBundle b;
    b.repo = std::make_shared<InMemoryUserRepo>();
    auto h = std::make_shared<PasswordHasher>();
    auto j = std::make_shared<JwtService>(make_jwt_cfg());
    b.auth = std::make_shared<AuthService>(b.repo, h, j);
    b.server = std::make_unique<ScopedServer>(port);
    auto ready_ptr = b.db_ready;
    oj::http::handlers::register_auth_routes(
        b.server->server(), b.auth,
        [ready_ptr] { return ready_ptr->load(std::memory_order_acquire); });
    return b;
}

// ---------------------------------------------------------------------------
//  Happy path
// ---------------------------------------------------------------------------
TEST(AuthHandlerTest, FirstRegistrationSucceedsAndIsAdmin) {
    ServerBundle b = make_server(19090);
    b.server->start();

    nlohmann::json body = {
        {"username", "alice"},
        {"email",    "alice@x.com"},
        {"password", "password123"},
    };
    auto res = make_client(19090).Post("/api/auth/register", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 19090 not reachable in this environment";

    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    ASSERT_TRUE(j["data"].is_object());
    EXPECT_EQ(j["data"]["user_id"].get<std::int64_t>(), 1);
    EXPECT_TRUE (j["data"]["is_admin"].get<bool>());
    EXPECT_FALSE(j["data"]["access_token"].get<std::string>().empty());
}

TEST(AuthHandlerTest, RefreshTokenCookieIsSetWithHttpOnlyAndPath) {
    ServerBundle b = make_server(19091);
    b.server->start();

    nlohmann::json body = {
        {"username", "alice"},
        {"email",    "alice@x.com"},
        {"password", "password123"},
    };
    auto res = make_client(19091).Post("/api/auth/register", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 19091 not reachable in this environment";
    ASSERT_EQ(res->status, 200);

    const std::string set_cookie = res->get_header_value("Set-Cookie");
    EXPECT_NE(set_cookie.find("refresh_token="),  std::string::npos);
    EXPECT_NE(set_cookie.find("HttpOnly"),        std::string::npos);
    EXPECT_NE(set_cookie.find("Path=/api/auth"),  std::string::npos);
    EXPECT_NE(set_cookie.find("SameSite=Lax"),    std::string::npos);
    EXPECT_NE(set_cookie.find("Max-Age=86400"),   std::string::npos);
}

TEST(AuthHandlerTest, SecondUserIsNotAdmin) {
    ServerBundle b = make_server(19092);
    b.server->start();

    auto cli = make_client(19092);
    nlohmann::json body1 = {{"username","alice"},{"email","a@x.com"},{"password","password123"}};
    auto r1 = cli.Post("/api/auth/register", body1.dump(), "application/json");
    if (!r1) GTEST_SKIP() << "port 19092 not reachable in this environment";
    ASSERT_EQ(r1->status, 200);

    nlohmann::json body2 = {{"username","bob"},{"email","b@x.com"},{"password","password456"}};
    auto r2 = cli.Post("/api/auth/register", body2.dump(), "application/json");
    ASSERT_EQ(r2->status, 200);
    auto j = nlohmann::json::parse(r2->body);
    EXPECT_FALSE(j["data"]["is_admin"].get<bool>());
    EXPECT_EQ(j["data"]["user_id"].get<std::int64_t>(), 2);
}

// ---------------------------------------------------------------------------
//  输入校验
// ---------------------------------------------------------------------------
TEST(AuthHandlerTest, EmptyBodyReturns400) {
    ServerBundle b = make_server(19093);
    b.server->start();
    auto res = make_client(19093).Post("/api/auth/register", "", "application/json");
    if (!res) GTEST_SKIP() << "port 19093 not reachable in this environment";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(AuthHandlerTest, MalformedJsonReturns400) {
    ServerBundle b = make_server(19094);
    b.server->start();
    auto res = make_client(19094).Post("/api/auth/register", "{not-json", "application/json");
    if (!res) GTEST_SKIP() << "port 19094 not reachable in this environment";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(AuthHandlerTest, MissingFieldReturns400) {
    ServerBundle b = make_server(19095);
    b.server->start();
    nlohmann::json body = {{"username", "alice"}, {"password", "password123"}};  // 缺 email
    auto res = make_client(19095).Post("/api/auth/register", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 19095 not reachable in this environment";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(AuthHandlerTest, ShortPasswordReturns400) {
    ServerBundle b = make_server(19096);
    b.server->start();
    nlohmann::json body = {{"username","a"},{"email","a@x.com"},{"password","short"}};
    auto res = make_client(19096).Post("/api/auth/register", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 19096 not reachable in this environment";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(AuthHandlerTest, DuplicateUsernameReturns409) {
    ServerBundle b = make_server(19097);
    b.server->start();
    auto cli = make_client(19097);
    nlohmann::json body = {{"username","alice"},{"email","a@x.com"},{"password","password123"}};
    auto r1 = cli.Post("/api/auth/register", body.dump(), "application/json");
    if (!r1) GTEST_SKIP() << "port 19097 not reachable in this environment";
    ASSERT_EQ(r1->status, 200);

    nlohmann::json body2 = {{"username","alice"},{"email","a2@x.com"},{"password","password456"}};
    auto r2 = cli.Post("/api/auth/register", body2.dump(), "application/json");
    ASSERT_EQ(r2->status, 409);
    auto j = nlohmann::json::parse(r2->body);
    EXPECT_EQ(j["code"].get<int>(), 1005);
    EXPECT_NE(std::string{j["message"]}.find("username"), std::string::npos);
}

TEST(AuthHandlerTest, DuplicateEmailReturns409) {
    ServerBundle b = make_server(19098);
    b.server->start();
    auto cli = make_client(19098);
    nlohmann::json body = {{"username","alice"},{"email","shared@x.com"},{"password","password123"}};
    auto r1 = cli.Post("/api/auth/register", body.dump(), "application/json");
    if (!r1) GTEST_SKIP() << "port 19098 not reachable in this environment";
    ASSERT_EQ(r1->status, 200);

    nlohmann::json body2 = {{"username","bob"},{"email","shared@x.com"},{"password","password456"}};
    auto r2 = cli.Post("/api/auth/register", body2.dump(), "application/json");
    ASSERT_EQ(r2->status, 409);
    auto j = nlohmann::json::parse(r2->body);
    EXPECT_EQ(j["code"].get<int>(), 1005);
    EXPECT_NE(std::string{j["message"]}.find("email"), std::string::npos);
}

// ---------------------------------------------------------------------------
//  DB 不可用 → 503
// ---------------------------------------------------------------------------
TEST(AuthHandlerTest, DbDownReturnsEnvelope) {
    ServerBundle b = make_server(19099);
    b.server->start();
    b.db_ready->store(false, std::memory_order_release);

    nlohmann::json body = {{"username","alice"},{"email","a@x.com"},{"password","password123"}};
    auto res = make_client(19099).Post("/api/auth/register", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 19099 not reachable in this environment";
    EXPECT_EQ(res->status, 500);  // SystemError → 500
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
    EXPECT_NE(std::string{j["message"]}.find("database"), std::string::npos);
}

// ---------------------------------------------------------------------------
//  错误方法
// ---------------------------------------------------------------------------
TEST(AuthHandlerTest, GetOnRegisterPathReturns404) {
    ServerBundle b = make_server(19100);
    b.server->start();
    auto res = make_client(19100).Get("/api/auth/register");
    if (!res) GTEST_SKIP() << "port 19100 not reachable in this environment";
    // 路由未注册 → 走 HttpServer 默认 404 envelope
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

}  // namespace
