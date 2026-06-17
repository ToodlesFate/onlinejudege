// =============================================================================
//  test_problem_repo_mysql.cpp —— MysqlProblemRepo 集成测试（需要 MySQL）
//  默认 SKIP（环境无 MySQL 时不阻塞 CI）；设置环境变量 OJ_RUN_MYSQL_TESTS=1
//  即可启用。
//
//  覆盖：
//    - 字段序列化 / 枚举往返 (Difficulty, ISO 8601 时间)
//    - find_by_id / create / update / soft_delete / set_published
//    - list 全功能：分页 / 难度过滤 / 标题搜索 / tag AND 过滤 / 排序
//    - 关联：MysqlTagRepo::set_problem_tags 后 list 能拉出 tags
//    - submission_stats 正确性（用 submissions 表）
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>

#include "common/config.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_types.hpp"
#include "domain/tag_repository.hpp"
#include "infra/mysql_client.hpp"
#include "infra/problem_repo.hpp"
#include "infra/tag_repo.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::ITagRepository;
using oj::domain::Problem;
using oj::domain::ProblemListItem;
using oj::domain::ProblemListQuery;
using oj::domain::Tag;
using oj::infra::MysqlClient;
using oj::infra::MysqlProblemRepo;
using oj::infra::MysqlTagRepo;

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
    c.pool_size = 2;
    c.connect_timeout_sec = 5;
    return c;
}

class MysqlProblemRepoTest : public ::testing::Test {
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
        repo_   = std::make_shared<MysqlProblemRepo>(cli_);
        tags_   = std::make_shared<MysqlTagRepo>(cli_);
        // 确保 tags 表已 seed（002_seed.sql）
        seed_tags_if_needed();
        // 准备一个"测试用户"（FK created_by 必须有 user）—— 这里直接用 id=1
        // SPEC §1.1 admin 必然存在
    }

    void TearDown() override {
        if (!cli_ || !cli_->is_ready()) return;
        // 清理本次测试创建的所有 problem
        try {
            auto lease = cli_->acquire();
            const std::string sql = "DELETE FROM problems WHERE title LIKE 'oj_test_%'";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        } catch (...) {}
    }

    // 把 002_seed.sql 的 8 个 tag 灌进去（如果不在）
    void seed_tags_if_needed() {
        auto existing = tags_->list_all();
        if (existing.size() >= 8) return;
        // 用裸 SQL 兜底
        auto lease = cli_->acquire();
        const std::string sqls[] = {
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (1,'数组','数组')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (2,'字符串','string')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (3,'链表','linked-list')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (4,'栈/队列','stack-queue')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (5,'树','tree')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (6,'图','graph')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (7,'动态规划','dp')",
            "INSERT IGNORE INTO tags (id,name,slug) VALUES (8,'贪心','greedy')",
        };
        for (const auto& s : sqls) {
            mysql_real_query(lease.raw(), s.data(), static_cast<unsigned long>(s.size()));
        }
    }

    // 创建一个 admin user 占位 (如果还没有)
    void ensure_admin_user() {
        auto lease = cli_->acquire();
        const std::string sql =
            "INSERT IGNORE INTO users (id,username,email,password_hash,is_admin) "
            "VALUES (1, 'oj_test_admin', 'admin@test.local', 'x', 1)";
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
    }

    Problem mkProblem(const std::string& title, Difficulty d, bool published,
                      int tl = 2000, int ml = 256, int ol = 64) {
        ensure_admin_user();
        Problem p;
        p.title = title;
        p.content_md = "# 描述\n\n输入两个数 a, b，输出 a + b。\n\n## 样例\n\n1 2\n3\n";
        p.difficulty = d;
        p.time_limit_ms = tl;
        p.memory_limit_mb = ml;
        p.output_limit_mb = ol;
        p.is_published = published;
        p.created_by = 1;  // admin user
        return p;
    }

    std::shared_ptr<MysqlClient>       cli_;
    std::shared_ptr<IProblemRepository> repo_;
    std::shared_ptr<ITagRepository>     tags_;
};

// ---------------------------------------------------------------------------
//  基本 CRUD
// ---------------------------------------------------------------------------
TEST_F(MysqlProblemRepoTest, CreateReturnsAutoIdAndCreatedAt) {
    auto p = repo_->create(mkProblem("oj_test_basic", Difficulty::Easy, true));
    EXPECT_GT(p.id, 0);
    EXPECT_FALSE(p.created_at.empty());
    // ISO 8601: "YYYY-MM-DDTHH:MM:SSZ"
    EXPECT_EQ(p.created_at.size(), 20u);
    EXPECT_EQ(p.created_at[10], 'T');
    EXPECT_EQ(p.created_at.back(), 'Z');
}

TEST_F(MysqlProblemRepoTest, FindByIdRoundTripsAllFields) {
    auto p = repo_->create(mkProblem("oj_test_roundtrip", Difficulty::Medium, true,
                                     /*tl*/3000, /*ml*/512, /*ol*/128));
    auto got = repo_->find_by_id(p.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->id,             p.id);
    EXPECT_EQ(got->title,          "oj_test_roundtrip");
    EXPECT_EQ(got->difficulty,     Difficulty::Medium);
    EXPECT_EQ(got->time_limit_ms,  3000);
    EXPECT_EQ(got->memory_limit_mb, 512);
    EXPECT_EQ(got->output_limit_mb, 128);
    EXPECT_TRUE(got->is_published);
}

TEST_F(MysqlProblemRepoTest, FindByIdReturnsNulloptForMissing) {
    auto got = repo_->find_by_id(999999999);
    EXPECT_FALSE(got.has_value());
}

TEST_F(MysqlProblemRepoTest, UpdateChangesFields) {
    auto p = repo_->create(mkProblem("oj_test_update", Difficulty::Easy, true));
    p.title = "oj_test_updated";
    p.difficulty = Difficulty::Hard;
    p.time_limit_ms = 5000;
    repo_->update(p);
    auto got = repo_->find_by_id(p.id);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->title, "oj_test_updated");
    EXPECT_EQ(got->difficulty, Difficulty::Hard);
    EXPECT_EQ(got->time_limit_ms, 5000);
}

TEST_F(MysqlProblemRepoTest, UpdateNonexistentThrows) {
    Problem p = mkProblem("nonexistent", Difficulty::Easy, true);
    p.id = 999999999;
    EXPECT_THROW(repo_->update(p), std::runtime_error);
}

TEST_F(MysqlProblemRepoTest, SetPublishedAndSoftDelete) {
    auto p = repo_->create(mkProblem("oj_test_publish", Difficulty::Easy, false));
    EXPECT_FALSE(repo_->find_by_id(p.id)->is_published);

    repo_->set_published(p.id, true);
    EXPECT_TRUE(repo_->find_by_id(p.id)->is_published);

    repo_->soft_delete(p.id);
    EXPECT_FALSE(repo_->find_by_id(p.id)->is_published);
}

// ---------------------------------------------------------------------------
//  list 过滤 / 分页 / 排序
// ---------------------------------------------------------------------------
TEST_F(MysqlProblemRepoTest, ListDefaultsHideUnpublished) {
    auto a = repo_->create(mkProblem("oj_test_pub_yes", Difficulty::Easy, true));
    auto b = repo_->create(mkProblem("oj_test_pub_no",  Difficulty::Easy, false));

    ProblemListQuery q;
    q.q = "oj_test_pub";
    auto r = repo_->list(q);
    bool found_pub = false, found_unpub = false;
    for (const auto& it : r.items) {
        if (it.id == a.id) found_pub = true;
        if (it.id == b.id) found_unpub = true;
    }
    EXPECT_TRUE(found_pub);
    EXPECT_FALSE(found_unpub);
}

TEST_F(MysqlProblemRepoTest, ListAdminCanSeeUnpublished) {
    auto a = repo_->create(mkProblem("oj_test_admin_visible", Difficulty::Easy, false));
    ProblemListQuery q;
    q.q = "oj_test_admin_visible";
    q.include_unpublished = true;
    auto r = repo_->list(q);
    bool found = false;
    for (const auto& it : r.items) if (it.id == a.id) found = true;
    EXPECT_TRUE(found);
}

TEST_F(MysqlProblemRepoTest, ListFilterByDifficulty) {
    auto easy = repo_->create(mkProblem("oj_test_diff_easy",   Difficulty::Easy,   true));
    auto hard = repo_->create(mkProblem("oj_test_diff_hard",   Difficulty::Hard,   true));

    ProblemListQuery q;
    q.q = "oj_test_diff_";
    q.difficulty = Difficulty::Hard;
    auto r = repo_->list(q);
    bool found_easy = false, found_hard = false;
    for (const auto& it : r.items) {
        if (it.id == easy.id) found_easy = true;
        if (it.id == hard.id) found_hard = true;
    }
    EXPECT_FALSE(found_easy);
    EXPECT_TRUE(found_hard);
}

TEST_F(MysqlProblemRepoTest, ListTitleSearchBySubstring) {
    auto a = repo_->create(mkProblem("oj_test_apple",  Difficulty::Easy, true));
    auto b = repo_->create(mkProblem("oj_test_banana", Difficulty::Easy, true));

    ProblemListQuery q;
    q.q = "banana";
    auto r = repo_->list(q);
    bool found_a = false, found_b = false;
    for (const auto& it : r.items) {
        if (it.id == a.id) found_a = true;
        if (it.id == b.id) found_b = true;
    }
    EXPECT_FALSE(found_a);
    EXPECT_TRUE(found_b);
}

TEST_F(MysqlProblemRepoTest, ListTagFilterIsAnd) {
    auto p_arr = repo_->create(mkProblem("oj_test_tag_arr",     Difficulty::Easy, true));
    auto p_str = repo_->create(mkProblem("oj_test_tag_str",     Difficulty::Easy, true));
    auto p_two = repo_->create(mkProblem("oj_test_tag_two",     Difficulty::Easy, true));
    auto p_non = repo_->create(mkProblem("oj_test_tag_none",    Difficulty::Easy, true));

    tags_->set_problem_tags(p_arr.id, {1});                 // 数组
    tags_->set_problem_tags(p_str.id, {2});                 // 字符串
    tags_->set_problem_tags(p_two.id, {1, 2});              // 数组 + 字符串
    tags_->set_problem_tags(p_non.id, {3});                 // 链表

    ProblemListQuery q;
    q.q = "oj_test_tag_";
    // 筛 "数组" → 应只剩 p_arr + p_two
    q.tag_slugs = {"数组"};
    auto r = repo_->list(q);
    std::set<std::int64_t> ids;
    for (const auto& it : r.items) ids.insert(it.id);
    EXPECT_TRUE(ids.count(p_arr.id) == 1);
    EXPECT_TRUE(ids.count(p_two.id) == 1);
    EXPECT_TRUE(ids.count(p_str.id) == 0);
    EXPECT_TRUE(ids.count(p_non.id) == 0);

    // 筛 "数组 AND 字符串" → 应只剩 p_two
    // 注：tag 2 的 slug 在 002_seed.sql 里是 'string'（不是中文 "字符串"），
    //     见 SPEC §4.2 备注里的笔误说明
    q.tag_slugs = {"数组", "string"};
    r = repo_->list(q);
    ids.clear();
    for (const auto& it : r.items) ids.insert(it.id);
    EXPECT_EQ(ids.size(), 1u);
    EXPECT_TRUE(ids.count(p_two.id) == 1);
}

TEST_F(MysqlProblemRepoTest, ListPaginationRespectsPageSize) {
    // 插 5 个
    std::vector<Problem> ps;
    for (int i = 0; i < 5; ++i) {
        ps.push_back(repo_->create(
            mkProblem("oj_test_pg_" + std::to_string(i), Difficulty::Easy, true)));
    }
    ProblemListQuery q;
    q.q = "oj_test_pg_";
    q.page_size = 2;
    q.page = 1;
    auto r1 = repo_->list(q);
    EXPECT_EQ(static_cast<int>(r1.items.size()), 2);
    EXPECT_EQ(r1.page, 1);
    EXPECT_GE(r1.total, 5);

    q.page = 2;
    auto r2 = repo_->list(q);
    EXPECT_EQ(static_cast<int>(r2.items.size()), 2);
    q.page = 3;
    auto r3 = repo_->list(q);
    EXPECT_GE(static_cast<int>(r3.items.size()), 1);
}

TEST_F(MysqlProblemRepoTest, ListSortByCreatedDesc) {
    auto a = repo_->create(mkProblem("oj_test_sort_a", Difficulty::Easy, true));
    auto b = repo_->create(mkProblem("oj_test_sort_b", Difficulty::Easy, true));
    ProblemListQuery q;
    q.q = "oj_test_sort_";
    q.sort = ProblemListQuery::Sort::CreatedDesc;
    auto r = repo_->list(q);
    ASSERT_GE(static_cast<int>(r.items.size()), 2);
    // b 比 a 后建，应排在前面
    EXPECT_EQ(r.items[0].id, b.id);
    EXPECT_EQ(r.items[1].id, a.id);
}

TEST_F(MysqlProblemRepoTest, ListItemCarriesTagList) {
    auto p = repo_->create(mkProblem("oj_test_with_tags", Difficulty::Easy, true));
    tags_->set_problem_tags(p.id, {1, 5});  // 数组 + 树
    ProblemListQuery q;
    q.q = "oj_test_with_tags";
    q.include_unpublished = true;
    auto r = repo_->list(q);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].tags.size(), 2u);
    EXPECT_EQ(r.items[0].tags[0].id, 1);
    EXPECT_EQ(r.items[0].tags[1].id, 5);
}

TEST_F(MysqlProblemRepoTest, SubmissionStatsInitiallyZero) {
    auto p = repo_->create(mkProblem("oj_test_stats_zero", Difficulty::Easy, true));
    auto [t, a] = repo_->submission_stats(p.id);
    EXPECT_EQ(t, 0);
    EXPECT_EQ(a, 0);
}

TEST_F(MysqlProblemRepoTest, SubmissionStatsAggregatesAcAndTotal) {
    auto p = repo_->create(mkProblem("oj_test_stats_agg", Difficulty::Easy, true));
    // 直接灌 submissions
    {
        auto lease = cli_->acquire();
        const std::string sqls[] = {
            "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
            "VALUES (1," + std::to_string(p.id) + ",'cpp','x','finished','AC')",
            "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
            "VALUES (1," + std::to_string(p.id) + ",'cpp','x','finished','AC')",
            "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
            "VALUES (1," + std::to_string(p.id) + ",'cpp','x','finished','WA')",
            "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
            "VALUES (1," + std::to_string(p.id) + ",'cpp','x','queued',NULL)",  // 不算 finished
        };
        for (const auto& s : sqls) {
            mysql_real_query(lease.raw(), s.data(), static_cast<unsigned long>(s.size()));
        }
    }
    auto [t, a] = repo_->submission_stats(p.id);
    EXPECT_EQ(t, 3);   // 2 AC + 1 WA
    EXPECT_EQ(a, 2);
    // 清理
    {
        auto lease = cli_->acquire();
        const std::string sql = "DELETE FROM submissions WHERE problem_id=" + std::to_string(p.id);
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
    }
}

TEST_F(MysqlProblemRepoTest, ListSortByPassRateOrdersHigherFirst) {
    // p_low  : 1 submission, 1 AC   → rate = 1.0
    // p_high : 10 submissions, 2 AC  → rate = 0.2
    auto p_low  = repo_->create(mkProblem("oj_test_rate_low",  Difficulty::Easy, true));
    auto p_high = repo_->create(mkProblem("oj_test_rate_high", Difficulty::Easy, true));
    {
        auto lease = cli_->acquire();
        for (int i = 0; i < 1; ++i) {
            const std::string sql =
                "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
                "VALUES (1," + std::to_string(p_low.id) + ",'cpp','x','finished','AC')";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        }
        for (int i = 0; i < 10; ++i) {
            const std::string sql =
                "INSERT INTO submissions (user_id, problem_id, language, code, status, result) "
                "VALUES (1," + std::to_string(p_high.id) + ",'cpp','x',"
                + (i < 2 ? "'finished','AC'" : "'finished','WA'") + ")";
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        }
    }
    ProblemListQuery q;
    q.q = "oj_test_rate_";
    q.sort = ProblemListQuery::Sort::PassRateDesc;
    q.include_unpublished = true;
    auto r = repo_->list(q);
    ASSERT_GE(r.items.size(), 2u);
    // 通过率高的应排在前面
    EXPECT_EQ(r.items[0].id, p_low.id);
    EXPECT_DOUBLE_EQ(r.items[0].pass_rate(), 1.0);
    EXPECT_DOUBLE_EQ(r.items[1].pass_rate(), 0.2);

    // 清理
    {
        auto lease = cli_->acquire();
        for (auto pid : {p_low.id, p_high.id}) {
            const std::string sql = "DELETE FROM submissions WHERE problem_id=" + std::to_string(pid);
            mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
        }
    }
}

}  // namespace
