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
#include "infra/testcase_repo.hpp"

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
        svc_ = std::make_shared<ProblemService>(repo_,
            std::make_shared<oj::infra::MysqlTestcaseRepo>(cli_),
            std::make_shared<oj::infra::MysqlTagRepo>(cli_));
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

// ===========================================================================
//  GET /api/problems/:id 端到端 MySQL 测试
// ===========================================================================
TEST_F(T, EndToEndDetailReturnsFullData) {
    auto pid = mkP("oj_api_detail_full", Difficulty::Medium, true, {1, 5});
    // 灌 2 sample + 1 hidden
    {
        auto lease = cli_->acquire();
        for (int i = 1; i <= 3; ++i) {
            const std::string sql =
                "INSERT INTO testcases (problem_id, case_index, input, expected_output, is_sample, score) VALUES ("
                + std::to_string(pid) + "," + std::to_string(i) + ","
                + "'in" + std::to_string(i) + "','out" + std::to_string(i) + "',"
                + (i <= 2 ? "1" : "0") + "," + (i <= 2 ? "40" : "20") + ")";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        }
    }
    ScopedServer srv(19700);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19700).Get("/api/problems/" + std::to_string(pid));
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["data"]["id"].get<std::int64_t>(), pid);
    EXPECT_EQ(j["data"]["title"], "oj_api_detail_full");
    EXPECT_EQ(j["data"]["difficulty"], "medium");
    EXPECT_EQ(j["data"]["is_published"], true);
    EXPECT_EQ(j["data"]["time_limit_ms"].get<int>(), 2000);
    EXPECT_EQ(j["data"]["memory_limit_mb"].get<int>(), 256);
    EXPECT_EQ(j["data"]["output_limit_mb"].get<int>(), 64);
    // tags
    ASSERT_EQ(j["data"]["tags"].size(), 2u);
    EXPECT_EQ(j["data"]["tags"][0]["id"].get<int>(), 1);
    EXPECT_EQ(j["data"]["tags"][1]["id"].get<int>(), 5);
    // samples：只 2 个
    ASSERT_EQ(j["data"]["sample_testcases"].size(), 2u);
    EXPECT_EQ(j["data"]["sample_testcases"][0]["case_index"].get<int>(), 1);
    EXPECT_EQ(j["data"]["sample_testcases"][0]["input"], "in1");
    EXPECT_EQ(j["data"]["sample_testcases"][0]["expected_output"], "out1");
    EXPECT_TRUE(j["data"]["sample_testcases"][0]["is_sample"].get<bool>());
    EXPECT_EQ(j["data"]["sample_testcases"][0]["score"].get<int>(), 40);
    EXPECT_EQ(j["data"]["sample_testcases"][1]["case_index"].get<int>(), 2);
    // hidden 不出现
    for (const auto& c : j["data"]["sample_testcases"]) {
        EXPECT_NE(c["case_index"].get<int>(), 3);
    }
}

TEST_F(T, EndToEndDetailReturns404ForMissing) {
    ScopedServer srv(19701);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19701).Get("/api/problems/99999999");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST_F(T, EndToEndDetailReturns404ForUnpublished) {
    mkP("oj_api_detail_draft", Difficulty::Easy, /*pub=*/false);
    // 找出刚插入的 id
    std::int64_t id = 0;
    {
        auto lease = cli_->acquire();
        const std::string sql = "SELECT id FROM problems WHERE title='oj_api_detail_draft'";
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        MYSQL_RES* r = mysql_store_result(lease.raw());
        if (r) {
            MYSQL_ROW row = mysql_fetch_row(r);
            if (row && row[0]) id = std::stoll(row[0]);
            mysql_free_result(r);
        }
    }
    ASSERT_GT(id, 0);

    ScopedServer srv(19702);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19702).Get("/api/problems/" + std::to_string(id));
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 404);
}

TEST_F(T, EndToEndDetailInvalidIdReturns400) {
    ScopedServer srv(19703);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return cli_->is_ready(); });
    srv.start();

    auto res = cli(19703).Get("/api/problems/abc");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST_F(T, EndToEndDetailDbDownReturns1008) {
    ScopedServer srv(19704);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return false; /* DB down */ });
    srv.start();

    auto res = cli(19704).Get("/api/problems/1");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
}

// ===========================================================================
//  GET /api/tags —— SPEC §5.2.2  端到端 (真 MySQL + 真 HttpServer)
// ===========================================================================
TEST_F(T, EndToEndTagsReturns8Presets) {
    ScopedServer srv(19705);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return true; });
    srv.start();

    auto res = cli(19705).Get("/api/tags");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    ASSERT_TRUE(j["data"].is_array());
    // 种子数据里有 8 个 tag
    ASSERT_EQ(j["data"].size(), 8u);
    // 按 id ASC
    for (int i = 0; i < 8; ++i) {
        int got = j["data"][i]["id"].get<int>();
        EXPECT_EQ(got, i + 1) << j["data"].dump();
    }
    // 字段形状
    for (const auto& t : j["data"]) {
        ASSERT_TRUE(t.contains("id"))   << t.dump();
        ASSERT_TRUE(t.contains("name")) << t.dump();
        ASSERT_TRUE(t.contains("slug")) << t.dump();
    }
    // 抽样校验（与 sql/002_seed.sql 一致）
    EXPECT_EQ(j["data"][0]["name"], "数组");
    EXPECT_EQ(j["data"][0]["slug"], "数组");
    EXPECT_EQ(j["data"][1]["name"], "字符串");
    EXPECT_EQ(j["data"][1]["slug"], "string");
    EXPECT_EQ(j["data"][6]["name"], "动态规划");
    EXPECT_EQ(j["data"][7]["name"], "贪心");
}

TEST_F(T, EndToEndTagsDbDownReturns1008) {
    ScopedServer srv(19706);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return false; /* DB down */ });
    srv.start();

    auto res = cli(19706).Get("/api/tags");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
}

TEST_F(T, EndToEndTagsPostReturns404) {
    ScopedServer srv(19707);
    oj::http::handlers::register_problem_routes(
        srv.server(), svc_, [this] { return true; });
    srv.start();

    auto res = cli(19707).Post("/api/tags", "{}", "application/json");
    ASSERT_TRUE(res != nullptr);
    EXPECT_EQ(res->status, 404);
}

}  // namespace
