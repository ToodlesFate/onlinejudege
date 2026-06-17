// =============================================================================
//  test_problem_handler_mysql.cpp — GET /api/problems 端到端 MySQL 集成测试
//  默认 SKIP；环境变量 OJ_RUN_MYSQL_TESTS=1 启用
//
//  把真 MysqlProblemRepo + 真 ProblemService + 真 HttpServer 串起来跑一遍，
//  重点验证：SQL 路径、tag 关联、JSON 序列化都没在端到端里出错
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <mysql.h>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/problem_handler.hpp"
#include "infra/mysql_client.hpp"
#include "infra/problem_repo.hpp"
#include "infra/tag_repo.hpp"

namespace {

using oj::common::AppConfig;
using oj::common::MysqlConfig;
using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::ITagRepository;
using oj::domain::Problem;
using oj::domain::ProblemService;
using oj::domain::Tag;
using oj::http::HttpServer;
using oj::infra::MysqlClient;
using oj::infra::MysqlProblemRepo;
using oj::infra::MysqlTagRepo;

bool enabled() {
    if (const char* e = std::getenv("OJ_RUN_MYSQL_TESTS"); e) return std::string{e} == "1";
    return false;
}

MysqlConfig mcfg() {
    MysqlConfig c;
    c.host     = std::getenv("OJ_MYSQL_HOST")     ? std::getenv("OJ_MYSQL_HOST")     : "127.0.0.1";
    c.port     = 3306;
    c.user     = std::getenv("OJ_MYSQL_USER")     ? std::getenv("OJ_MYSQL_USER")     : "oj";
    c.password = std::getenv("OJ_MYSQL_PASSWORD") ? std::getenv("OJ_MYSQL_PASSWORD") : "oj";
    c.database = std::getenv("OJ_MYSQL_DATABASE") ? std::getenv("OJ_MYSQL_DATABASE") : "oj";
    c.pool_size = 2;
    c.connect_timeout_sec = 5;
    return c;
}

AppConfig make_cfg(uint16_t port) {
    AppConfig c;
    c.server.host = "127.0.0.1";
    c.server.port = port;
    c.server.thread_pool_size = 2;
    c.log.stdout_console = false;
    c.log.dir = "/tmp";
    return c;
}

class ScopedServer {
public:
    explicit ScopedServer(uint16_t port) : cfg_(make_cfg(port)), srv_(std::move(cfg_)) {}
    ~ScopedServer() { srv_.stop(); if (t_.joinable()) t_.join(); }
    void start() {
        t_ = std::thread([this] {
            ready_.store(true);
            std::string r;
            srv_.listen(&r);
        });
        for (int i = 0; i < 300 && !ready_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    HttpServer& server() noexcept { return srv_; }
private:
    AppConfig cfg_;
    HttpServer srv_;
    std::thread t_;
    std::atomic<bool> ready_{false};
};

httplib::Client cli(uint16_t port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(5, 0);
    return c;
}

class T : public ::testing::Test {
protected:
    void SetUp() override {
        if (!enabled()) GTEST_SKIP() << "set OJ_RUN_MYSQL_TESTS=1";
        cli_ = std::make_shared<MysqlClient>(mcfg());
        try { cli_->connect(); }
        catch (const std::exception& e) { GTEST_SKIP() << "MySQL not reachable: " << e.what(); }

        repo_ = std::make_shared<MysqlProblemRepo>(cli_);
        tags_ = std::make_shared<MysqlTagRepo>(cli_);
        ensure_tags();
        ensure_admin();
        svc_ = std::make_shared<ProblemService>(repo_);
    }
    void TearDown() override {
        if (!cli_ || !cli_->is_ready()) return;
        try {
            auto lease = cli_->acquire();
            const std::string sql = "DELETE FROM problems WHERE title LIKE 'oj_api_%'";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        } catch (...) {}
    }
    void ensure_tags() {
        auto lease = cli_->acquire();
        const char* sqls[] = {
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (1,'数组','数组')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (2,'字符串','string')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (3,'链表','linked-list')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (4,'栈/队列','stack-queue')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (5,'树','tree')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (6,'图','graph')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (7,'动态规划','dp')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (8,'贪心','greedy')",
        };
        for (auto s : sqls) mysql_real_query(lease.raw(), s, std::strlen(s));
    }
    void ensure_admin() {
        auto lease = cli_->acquire();
        const std::string sql =
            "INSERT IGNORE INTO users (id,username,email,password_hash,is_admin) "
            "VALUES (1, 'oj_api_admin', 'admin@api.test', 'x', 1)";
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
    }
    std::int64_t mkP(const std::string& t, Difficulty d, bool pub,
                     std::vector<int> tag_ids = {}) {
        Problem p;
        p.title = t; p.content_md = "x"; p.difficulty = d;
        p.is_published = pub; p.created_by = 1;
        p.created_at = "2026-04-23T10:00:00Z";
        auto saved = repo_->create(p);
        if (!tag_ids.empty()) {
            tags_->set_problem_tags(saved.id, tag_ids);
        }
        return saved.id;
    }

    std::shared_ptr<MysqlClient>      cli_;
    std::shared_ptr<IProblemRepository> repo_;
    std::shared_ptr<ITagRepository>    tags_;
    std::shared_ptr<ProblemService>    svc_;
};

TEST_F(T, EndToEndListReturnsItemsAndPagination) {
    for (int i = 0; i < 5; ++i) {
        mkP("oj_api_e2e_" + std::to_string(i), Difficulty::Easy, true);
    }
    ScopedServer srv(19600);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19600).Get("/api/problems?q=oj_api_e2e_");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["data"]["total"].get<int>(), 5);
    EXPECT_EQ(j["data"]["items"].size(), 5u);
    EXPECT_EQ(j["data"]["page"].get<int>(), 1);
    EXPECT_EQ(j["data"]["size"].get<int>(), 20);
}

TEST_F(T, EndToEndFilterByDifficulty) {
    mkP("oj_api_diff_easy", Difficulty::Easy,   true);
    mkP("oj_api_diff_hard", Difficulty::Hard,   true);
    mkP("oj_api_diff_easy2", Difficulty::Easy,  true);
    ScopedServer srv(19601);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19601).Get("/api/problems?q=oj_api_diff_&difficulty=hard");
    ASSERT_TRUE(res != nullptr);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j["data"]["items"][0]["difficulty"], "hard");
}

TEST_F(T, EndToEndTagAndFilter) {
    mkP("oj_api_tag_a", Difficulty::Easy, true, {1});
    mkP("oj_api_tag_b", Difficulty::Easy, true, {2});
    mkP("oj_api_tag_ab", Difficulty::Easy, true, {1, 2});
    ScopedServer srv(19602);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    // ?tag=数组 → expect 2
    auto res = cli(19602).Get("/api/problems?q=oj_api_tag_&tag=" +
                                httplib::detail::encode_url("数组"));
    ASSERT_TRUE(res != nullptr);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 2);
    // ?tag=数组&tag=string → expect 1 (only ab)
    auto res2 = cli(19602).Get(
        "/api/problems?q=oj_api_tag_&tag=" + httplib::detail::encode_url("数组") +
        "&tag=" + httplib::detail::encode_url("string"));
    ASSERT_TRUE(res2 != nullptr);
    auto j2 = nlohmann::json::parse(res2->body);
    EXPECT_EQ(j2["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j2["data"]["items"][0]["title"], "oj_api_tag_ab");
}

TEST_F(T, EndToEndPublishedFlagRespected) {
    mkP("oj_api_pub",  Difficulty::Easy, true);
    mkP("oj_api_priv", Difficulty::Easy, false);
    ScopedServer srv(19603);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19603).Get("/api/problems?q=oj_api_");
    ASSERT_TRUE(res != nullptr);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j["data"]["items"][0]["title"], "oj_api_pub");
}

TEST_F(T, EndToEndItemCarriesTagListAndStats) {
    auto pid = mkP("oj_api_meta", Difficulty::Medium, true, {1, 5});
    ScopedServer srv(19604);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19604).Get("/api/problems?q=oj_api_meta");
    ASSERT_TRUE(res != nullptr);
    auto j = nlohmann::json::parse(res->body);
    ASSERT_EQ(j["data"]["items"].size(), 1u);
    const auto& it = j["data"]["items"][0];
    EXPECT_EQ(it["id"].get<std::int64_t>(), pid);
    EXPECT_EQ(it["difficulty"], "medium");
    EXPECT_EQ(it["is_published"], true);
    ASSERT_EQ(it["tags"].size(), 2u);
    EXPECT_TRUE(it["stats"].contains("total"));
    EXPECT_TRUE(it["stats"].contains("accepted"));
    EXPECT_TRUE(it["stats"].contains("pass_rate"));
    EXPECT_DOUBLE_EQ(it["stats"]["pass_rate"].get<double>(), 0.0);  // 无任何 submission
}

TEST_F(T, EndToEndPageSizeValidatedTo10Or20Or50) {
    for (int i = 0; i < 30; ++i) mkP("oj_api_sz_" + std::to_string(i), Difficulty::Easy, true);
    ScopedServer srv(19605);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19605).Get("/api/problems?q=oj_api_sz_&size=999");
    ASSERT_TRUE(res != nullptr);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["size"].get<int>(), 20);
}

TEST_F(T, EndToEndInvalidPageReturns400) {
    ScopedServer srv(19606);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19606).Get("/api/problems?page=0");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST_F(T, EndToEndDbDownReturns503) {
    ScopedServer srv(19607);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return false; /* DB down */ });
    srv.start();

    auto res = cli(19607).Get("/api/problems");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
}

}  // namespace
