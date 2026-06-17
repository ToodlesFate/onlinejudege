// =============================================================================
//  test_testcase_repo_mysql.cpp —— MysqlTestcaseRepo 集成测试
//  默认 SKIP；环境变量 OJ_RUN_MYSQL_TESTS=1 启用
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "common/config.hpp"
#include "domain/problem_types.hpp"
#include "domain/testcase_repository.hpp"
#include "infra/mysql_client.hpp"
#include "infra/problem_repo.hpp"
#include "infra/testcase_repo.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::domain::Difficulty;
using oj::domain::ITestcaseRepository;
using oj::domain::Problem;
using oj::domain::Testcase;
using oj::infra::MysqlClient;
using oj::infra::MysqlProblemRepo;
using oj::infra::MysqlTestcaseRepo;

bool enabled() {
    if (const char* e = std::getenv("OJ_RUN_MYSQL_TESTS"); e) return std::string{e} == "1";
    return false;
}

MysqlConfig cfg() {
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

class TestcaseRepoTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!enabled()) GTEST_SKIP() << "set OJ_RUN_MYSQL_TESTS=1";
        cli_ = std::make_shared<MysqlClient>(cfg());
        try { cli_->connect(); }
        catch (const std::exception& e) { GTEST_SKIP() << "MySQL not reachable: " << e.what(); }
        prob_ = std::make_shared<MysqlProblemRepo>(cli_);
        repo_ = std::make_shared<MysqlTestcaseRepo>(cli_);
        ensure_admin();
    }
    void TearDown() override {
        if (!cli_ || !cli_->is_ready()) return;
        try {
            auto lease = cli_->acquire();
            const std::string sql = "DELETE FROM problems WHERE title LIKE 'oj_tc_%'";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        } catch (...) {}
    }
    void ensure_admin() {
        auto lease = cli_->acquire();
        const std::string sql =
            "INSERT IGNORE INTO users (id,username,email,password_hash,is_admin) "
            "VALUES (1, 'oj_tc_admin', 'admin@tc.test', 'x', 1)";
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
    }
    std::int64_t mkProblem(const std::string& t) {
        Problem p;
        p.title = t;
        p.content_md = "x";
        p.difficulty = Difficulty::Easy;
        p.created_by = 1;
        return prob_->create(p).id;
    }

    std::shared_ptr<MysqlClient>        cli_;
    std::shared_ptr<MysqlProblemRepo>    prob_;
    std::shared_ptr<ITestcaseRepository> repo_;
};

TEST_F(TestcaseRepoTest, CreateManyAndListByProblem) {
    auto pid = mkProblem("oj_tc_basic");
    std::vector<Testcase> cases;
    for (int i = 0; i < 3; ++i) {
        Testcase c;
        c.case_index       = i + 1;
        c.input            = "in" + std::to_string(i);
        c.expected_output  = "out" + std::to_string(i);
        c.is_sample        = (i == 0);
        c.score            = 30;
        cases.push_back(c);
    }
    repo_->create_many(pid, cases);
    auto list = repo_->list_by_problem(pid);
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0].case_index, 1);
    EXPECT_EQ(list[2].case_index, 3);
    EXPECT_TRUE(list[0].is_sample);
    EXPECT_FALSE(list[1].is_sample);
}

TEST_F(TestcaseRepoTest, ListSamplesOnlyReturnsIsSample) {
    auto pid = mkProblem("oj_tc_samples");
    std::vector<Testcase> cases;
    for (int i = 1; i <= 4; ++i) {
        Testcase c;
        c.case_index = i;
        c.input = "x";
        c.expected_output = "y";
        c.is_sample = (i <= 2);
        c.score = 25;
        cases.push_back(c);
    }
    repo_->create_many(pid, cases);
    auto samples = repo_->list_samples(pid);
    EXPECT_EQ(samples.size(), 2u);
    EXPECT_TRUE(samples[0].is_sample);
    EXPECT_TRUE(samples[1].is_sample);
}

TEST_F(TestcaseRepoTest, ReplaceByProblemDeletesOldAndInsertsNew) {
    auto pid = mkProblem("oj_tc_replace");
    std::vector<Testcase> v1;
    for (int i = 1; i <= 3; ++i) {
        Testcase c; c.case_index = i; c.input = "old"; c.expected_output = "old"; c.score = 30; v1.push_back(c);
    }
    repo_->create_many(pid, v1);
    EXPECT_EQ(repo_->list_by_problem(pid).size(), 3u);

    std::vector<Testcase> v2;
    Testcase c1; c1.case_index = 1; c1.input = "new1"; c1.expected_output = "new1"; c1.score = 100; v2.push_back(c1);
    repo_->replace_by_problem(pid, v2);
    auto list = repo_->list_by_problem(pid);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].input, "new1");
    EXPECT_EQ(list[0].score, 100);
}

TEST_F(TestcaseRepoTest, DeleteByProblemRemovesAll) {
    auto pid = mkProblem("oj_tc_delete");
    std::vector<Testcase> v;
    for (int i = 1; i <= 5; ++i) {
        Testcase c; c.case_index = i; c.input = "x"; c.expected_output = "y"; c.score = 20; v.push_back(c);
    }
    repo_->create_many(pid, v);
    EXPECT_EQ(repo_->list_by_problem(pid).size(), 5u);
    repo_->delete_by_problem(pid);
    EXPECT_EQ(repo_->list_by_problem(pid).size(), 0u);
}

TEST_F(TestcaseRepoTest, CreateManyRejectsInvalidCaseIndex) {
    auto pid = mkProblem("oj_tc_bad_idx");
    std::vector<Testcase> v;
    Testcase c; c.case_index = 0;  // 无效
    c.input = "x"; c.expected_output = "y"; c.score = 10;
    v.push_back(c);
    EXPECT_THROW(repo_->create_many(pid, v), std::runtime_error);
}

TEST_F(TestcaseRepoTest, EmptyCreateManyIsNoOp) {
    auto pid = mkProblem("oj_tc_empty");
    EXPECT_NO_THROW(repo_->create_many(pid, {}));
    EXPECT_EQ(repo_->list_by_problem(pid).size(), 0u);
}

TEST_F(TestcaseRepoTest, LontTextRoundtripsUnchanged) {
    auto pid = mkProblem("oj_tc_lonttext");
    std::vector<Testcase> v;
    Testcase c;
    c.case_index = 1;
    c.input            = std::string(5000, 'A') + "\n" + std::string(5000, 'B');
    c.expected_output  = std::string(10000, 'C');
    c.score = 100;
    v.push_back(c);
    repo_->create_many(pid, v);
    auto list = repo_->list_by_problem(pid);
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].input,            c.input);
    EXPECT_EQ(list[0].expected_output,  c.expected_output);
}

}  // namespace
