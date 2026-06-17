// =============================================================================
//  test_submission_repo_mysql.cpp —— MysqlSubmissionRepo 集成测试（需要 MySQL）
//  默认 SKIP（环境无 MySQL 时不阻塞 CI）；设置环境变量 OJ_RUN_MYSQL_TESTS=1
//  即可启用。
//
//  覆盖：
//    - create / find_by_id 字段往返
//    - claim_one 把 status 从 queued 改成 running
//    - 多次 claim_one 不重复抢同一任务
//    - 4 worker 并发 claim 8 个任务，每个任务恰好被 1 个 worker 抢到
//    - finish 写终态字段（status/result/score/...）+ finished_at
//    - insert_case 写 submission_cases 行（user_output 仅 is_sample=1 存）
//    - mark_all_running_as_se_on_shutdown 把 running 全部改 SE
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "domain/submission_repository.hpp"
#include "domain/submission_types.hpp"
#include "infra/mysql_client.hpp"
#include "infra/submission_repo.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::domain::ClaimedTask;
using oj::domain::JudgeTaskPayload;
using oj::domain::Language;
using oj::domain::SubmissionCase;
using oj::domain::SubmissionResult;
using oj::infra::MysqlClient;
using oj::infra::MysqlSubmissionRepo;

bool mysql_tests_enabled() {
    if (const char* env = std::getenv("OJ_RUN_MYSQL_TESTS"); env != nullptr) {
        return std::string{env} == "1";
    }
    return false;
}

MysqlConfig make_cfg() {
    MysqlConfig c;
    c.host     = std::getenv("OJ_MYSQL_HOST")     ? std::getenv("OJ_MYSQL_HOST")     : "127.0.0.1";
    c.port     = 3306;
    c.user     = std::getenv("OJ_MYSQL_USER")     ? std::getenv("OJ_MYSQL_USER")     : "oj";
    c.password = std::getenv("OJ_MYSQL_PASSWORD") ? std::getenv("OJ_MYSQL_PASSWORD") : "oj";
    c.database = std::getenv("OJ_MYSQL_DATABASE") ? std::getenv("OJ_MYSQL_DATABASE") : "oj";
    c.pool_size = 4;
    c.connect_timeout_sec = 5;
    return c;
}

class MysqlSubmissionRepoTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysql_tests_enabled()) {
            GTEST_SKIP() << "set OJ_RUN_MYSQL_TESTS=1 to run MySQL integration tests";
        }
        cli_ = std::make_shared<MysqlClient>(make_cfg());
        try {
            cli_->connect();
        } catch (const std::exception& e) {
            GTEST_SKIP() << "MySQL not reachable: " << e.what();
        }
        repo_ = std::make_shared<MysqlSubmissionRepo>(cli_);
        cleanup();
        ensure_admin_user();
        ensure_problem();
    }

    void TearDown() override {
        cleanup();
    }

    void cleanup() {
        if (!cli_ || !cli_->is_ready()) return;
        auto lease = cli_->acquire();
        const std::string sqls[] = {
            "DELETE FROM submission_cases WHERE submission_id IN "
            "(SELECT id FROM submissions WHERE user_id=1)",
            "DELETE FROM submissions WHERE user_id=1",
            "DELETE FROM testcases WHERE problem_id=999999",
            "DELETE FROM problems WHERE id=999999",
        };
        for (const auto& s : sqls) {
            mysql_real_query(lease.raw(), s.data(),
                             static_cast<unsigned long>(s.size()));
        }
    }

    void ensure_admin_user() {
        auto lease = cli_->acquire();
        const std::string sql =
            "INSERT IGNORE INTO users (id,username,email,password_hash,is_admin) "
            "VALUES (1, 'oj_test_admin', 'admin@test.local', 'x', 1)";
        mysql_real_query(lease.raw(), sql.data(),
                         static_cast<unsigned long>(sql.size()));
    }

    void ensure_problem() {
        auto lease = cli_->acquire();
        const std::string sqls[] = {
            "INSERT IGNORE INTO problems (id,title,content_md,difficulty,"
            "time_limit_ms,memory_limit_mb,output_limit_mb,is_published,created_by) "
            "VALUES (999999, 'oj_test_problem', 'x', 'easy', 2000, 256, 64, 1, 1)",
            "INSERT IGNORE INTO testcases (problem_id,case_index,input,"
            "expected_output,is_sample,score) VALUES "
            "(999999, 1, '1 2\\n', '3\\n', 1, 50),"
            "(999999, 2, '5 7\\n', '12\\n', 0, 50)",
        };
        for (const auto& s : sqls) {
            mysql_real_query(lease.raw(), s.data(),
                             static_cast<unsigned long>(s.size()));
        }
    }

    std::shared_ptr<MysqlClient>       cli_;
    std::shared_ptr<MysqlSubmissionRepo> repo_;
};

// ---------------------------------------------------------------------------
//  create / find_by_id 字段往返
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, CreateAndFind) {
    auto id = repo_->create(/*user_id=*/1, /*problem_id=*/999999,
                            Language::Cpp, "int main(){return 0;}");
    EXPECT_GT(id, 0);
    auto sub = repo_->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->user_id,    1);
    EXPECT_EQ(sub->problem_id, 999999);
    EXPECT_EQ(sub->language,   Language::Cpp);
    EXPECT_EQ(sub->status,     oj::domain::SubmissionStatus::Queued);
    EXPECT_FALSE(sub->result.has_value());
    EXPECT_EQ(sub->code,       "int main(){return 0;}");
    EXPECT_FALSE(sub->created_at.empty());
}

// ---------------------------------------------------------------------------
//  claim_one 把 status 改成 running
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, ClaimOneChangesStatusToRunning) {
    auto id = repo_->create(1, 999999, Language::Java, "class Main{}");
    ClaimedTask t;
    EXPECT_TRUE(repo_->claim_one(t));
    EXPECT_EQ(t.submission_id, id);
    EXPECT_EQ(t.language,      Language::Java);
    auto sub = repo_->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->status, oj::domain::SubmissionStatus::Running);
}

TEST_F(MysqlSubmissionRepoTest, ClaimOneReturnsFalseWhenQueueEmpty) {
    ClaimedTask t;
    EXPECT_FALSE(repo_->claim_one(t));
}

TEST_F(MysqlSubmissionRepoTest, ClaimOneFifoOrder) {
    // 插入 3 条 submission，分别 sleep 一点时间让 created_at 严格递增
    auto id1 = repo_->create(1, 999999, Language::Cpp, "v1");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto id2 = repo_->create(1, 999999, Language::Java, "v2");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto id3 = repo_->create(1, 999999, Language::Python, "v3");

    ClaimedTask t;
    EXPECT_TRUE(repo_->claim_one(t));
    EXPECT_EQ(t.submission_id, id1);
    EXPECT_TRUE(repo_->claim_one(t));
    EXPECT_EQ(t.submission_id, id2);
    EXPECT_TRUE(repo_->claim_one(t));
    EXPECT_EQ(t.submission_id, id3);
}

// ---------------------------------------------------------------------------
//  并发 claim_one：8 worker 抢 8 个任务，每个任务恰好被 1 个 worker 抢到
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, ConcurrentClaimHasNoDuplicate) {
    constexpr int N = 8;
    std::vector<std::int64_t> ids;
    for (int i = 0; i < N; ++i) {
        ids.push_back(repo_->create(1, 999999, Language::Cpp, "code" + std::to_string(i)));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::mutex                          mu;
    std::set<std::int64_t>              seen;
    std::atomic<int>                    success{0};
    std::atomic<int>                    error{0};
    std::vector<std::thread>            ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&] {
            while (true) {
                ClaimedTask ct;
                try {
                    if (!repo_->claim_one(ct)) break;
                    std::lock_guard<std::mutex> lk(mu);
                    seen.insert(ct.submission_id);
                    ++success;
                } catch (...) { ++error; break; }
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(error.load(),   0);
    EXPECT_EQ(success.load(), N);
    EXPECT_EQ(seen.size(),    static_cast<std::size_t>(N));
    for (auto id : ids) EXPECT_TRUE(seen.count(id) > 0);
}

// ---------------------------------------------------------------------------
//  load_task：返回 code + limits + testcases
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, LoadTaskReturnsFullPayload) {
    auto id = repo_->create(1, 999999, Language::Python, "print(1+2)");
    ClaimedTask ct;
    ASSERT_TRUE(repo_->claim_one(ct));
    auto payload = repo_->load_task(ct.submission_id);
    EXPECT_EQ(payload.submission_id, ct.submission_id);
    EXPECT_EQ(payload.language,      Language::Python);
    EXPECT_EQ(payload.code,          "print(1+2)");
    EXPECT_EQ(payload.time_limit_ms,   2000);
    EXPECT_EQ(payload.memory_limit_mb, 256);
    EXPECT_EQ(payload.output_limit_mb, 64);
    ASSERT_EQ(payload.testcases.size(), 2u);
    EXPECT_EQ(payload.testcases[0].first,  "1 2\n");
    EXPECT_EQ(payload.testcases[0].second, "3\n");
}

TEST_F(MysqlSubmissionRepoTest, LoadTaskThrowsWhenSubmissionMissing) {
    EXPECT_THROW(repo_->load_task(99999999), std::runtime_error);
}

// ---------------------------------------------------------------------------
//  finish：写终态字段
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, FinishWritesTerminalState) {
    auto id = repo_->create(1, 999999, Language::Cpp, "x");
    ClaimedTask ct;
    ASSERT_TRUE(repo_->claim_one(ct));
    repo_->finish(ct.submission_id,
                  SubmissionResult::WA, /*score=*/40, /*time=*/1500, /*mem=*/8000,
                  /*compile_output=*/"", /*judge_message=*/"");
    auto sub = repo_->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->status, oj::domain::SubmissionStatus::Finished);
    ASSERT_TRUE(sub->result.has_value());
    EXPECT_EQ(*sub->result, SubmissionResult::WA);
    EXPECT_EQ(sub->total_score,    40);
    EXPECT_EQ(sub->time_used_ms,   1500);
    EXPECT_EQ(sub->memory_used_kb, 8000);
    EXPECT_FALSE(sub->finished_at.empty());
}

TEST_F(MysqlSubmissionRepoTest, FinishTruncatesLongJudgeMessage) {
    auto id = repo_->create(1, 999999, Language::Cpp, "x");
    ClaimedTask ct;
    ASSERT_TRUE(repo_->claim_one(ct));
    std::string long_msg(800, 'x');
    repo_->finish(ct.submission_id, SubmissionResult::SE, 0, 0, 0, "", long_msg);
    auto sub = repo_->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_LE(sub->judge_message.size(), 500u);
}

// ---------------------------------------------------------------------------
//  insert_case：user_output 仅 is_sample=1 持久化
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, InsertCaseOnlyPersistsUserOutputWhenSample) {
    auto id = repo_->create(1, 999999, Language::Cpp, "x");

    SubmissionCase c1;
    c1.submission_id = id;
    c1.case_index    = 1;
    c1.status        = SubmissionResult::AC;
    c1.time_used_ms  = 10;
    c1.memory_used_kb = 1024;
    c1.score         = 50;
    c1.is_sample     = true;
    c1.user_output   = "3\n";

    SubmissionCase c2 = c1;
    c2.case_index    = 2;
    c2.is_sample     = false;
    c2.user_output   = "SECRET_NOT_STORED";  // 不应被持久化

    repo_->insert_case(id, c1);
    repo_->insert_case(id, c2);

    auto detail = repo_->get_full(id);
    ASSERT_TRUE(detail.has_value());
    ASSERT_EQ(detail->cases.size(), 2u);
    EXPECT_EQ(detail->cases[0].user_output, "3\n");
    EXPECT_TRUE(detail->cases[1].user_output.empty());
}

// ---------------------------------------------------------------------------
//  update_status：finished 时自动写 finished_at
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, UpdateStatusFinishedSetsFinishedAt) {
    auto id = repo_->create(1, 999999, Language::Cpp, "x");
    ClaimedTask ct;
    ASSERT_TRUE(repo_->claim_one(ct));
    repo_->update_status(ct.submission_id, oj::domain::SubmissionStatus::Finished);
    auto sub = repo_->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->status, oj::domain::SubmissionStatus::Finished);
    EXPECT_FALSE(sub->finished_at.empty());
}

// ---------------------------------------------------------------------------
//  mark_all_running_as_se_on_shutdown
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, MarkAllRunningAsSE) {
    // 创建 3 个 running submission
    for (int i = 0; i < 3; ++i) {
        auto id = repo_->create(1, 999999, Language::Cpp, "x");
        ClaimedTask ct;
        ASSERT_TRUE(repo_->claim_one(ct));
    }
    repo_->mark_all_running_as_se_on_shutdown("dispatcher stopped");
    // 验证全部 finished + SE
    auto lease = cli_->acquire();
    const std::string sql =
        "SELECT COUNT(*) FROM submissions WHERE user_id=1 AND status='finished' AND result='SE'";
    ASSERT_EQ(mysql_real_query(lease.raw(), sql.data(),
                               static_cast<unsigned long>(sql.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long n = 0;
    if (auto row = mysql_fetch_row(r)) n = row[0] ? std::stoll(row[0]) : 0;
    mysql_free_result(r);
    EXPECT_EQ(n, 3);
}

// ---------------------------------------------------------------------------
//  list_by_user / list_public_accepted
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, ListByUserFiltersAndPages) {
    auto id1 = repo_->create(1, 999999, Language::Cpp,    "a");
    auto id2 = repo_->create(1, 999999, Language::Java,   "b");
    auto id3 = repo_->create(1, 999999, Language::Python, "c");

    // 把全部 finish 成 AC
    for (auto id : {id1, id2, id3}) {
        ClaimedTask ct;
        ASSERT_TRUE(repo_->claim_one(ct));
        repo_->finish(ct.submission_id, SubmissionResult::AC, 100, 0, 0, "", "");
    }

    oj::domain::SubmissionListQuery q;
    q.page      = 1;
    q.page_size = 10;
    q.user_id   = 1;
    auto r = repo_->list_by_user(q);
    EXPECT_EQ(r.total, 3);
    EXPECT_EQ(r.items.size(), 3u);

    q.language = Language::Java;
    auto r2 = repo_->list_by_user(q);
    EXPECT_EQ(r2.total, 1);
    EXPECT_EQ(r2.items[0].language, Language::Java);
}

TEST_F(MysqlSubmissionRepoTest, ListPublicAcceptedExcludesNonAC) {
    // 2 个 AC + 1 个 WA
    std::vector<std::int64_t> ids;
    for (int i = 0; i < 3; ++i) {
        auto id = repo_->create(1, 999999, Language::Cpp, "x");
        ClaimedTask ct;
        ASSERT_TRUE(repo_->claim_one(ct));
        repo_->finish(ct.submission_id,
                      i < 2 ? SubmissionResult::AC : SubmissionResult::WA,
                      i < 2 ? 100 : 0, 0, 0, "", "");
        ids.push_back(id);
    }

    oj::domain::SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = repo_->list_public_accepted(q);
    EXPECT_EQ(r.total, 2);
    for (auto& s : r.items) {
        ASSERT_TRUE(s.result.has_value());
        EXPECT_EQ(*s.result, SubmissionResult::AC);
    }
}

// ---------------------------------------------------------------------------
//  get_full：submission + cases
// ---------------------------------------------------------------------------
TEST_F(MysqlSubmissionRepoTest, GetFullReturnsCases) {
    auto id = repo_->create(1, 999999, Language::Cpp, "x");
    SubmissionCase c1; c1.submission_id = id; c1.case_index = 1;
    c1.status = SubmissionResult::AC; c1.is_sample = true;
    c1.user_output = "ok\n"; c1.score = 50;
    repo_->insert_case(id, c1);
    auto detail = repo_->get_full(id);
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->submission.id, id);
    ASSERT_EQ(detail->cases.size(), 1u);
    EXPECT_EQ(detail->cases[0].user_output, "ok\n");
}

}  // namespace