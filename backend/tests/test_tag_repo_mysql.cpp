// =============================================================================
//  test_tag_repo_mysql.cpp —— MysqlTagRepo 集成测试
//  默认 SKIP；环境变量 OJ_RUN_MYSQL_TESTS=1 启用
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <set>
#include <string>

#include "common/config.hpp"
#include "domain/problem_types.hpp"
#include "domain/tag_repository.hpp"
#include "infra/mysql_client.hpp"
#include "infra/problem_repo.hpp"
#include "infra/tag_repo.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::domain::Difficulty;
using oj::domain::ITagRepository;
using oj::domain::Problem;
using oj::domain::Tag;
using oj::infra::MysqlClient;
using oj::infra::MysqlProblemRepo;
using oj::infra::MysqlTagRepo;

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

class TagRepoTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!enabled()) GTEST_SKIP() << "set OJ_RUN_MYSQL_TESTS=1";
        cli_ = std::make_shared<MysqlClient>(cfg());
        try { cli_->connect(); }
        catch (const std::exception& e) { GTEST_SKIP() << "MySQL not reachable: " << e.what(); }
        repo_ = std::make_shared<MysqlTagRepo>(cli_);
        prob_ = std::make_shared<MysqlProblemRepo>(cli_);
        ensure_tags();
        ensure_admin();
    }
    void TearDown() override {
        if (!cli_ || !cli_->is_ready()) return;
        try {
            auto lease = cli_->acquire();
            const std::string sql = "DELETE FROM problems WHERE title LIKE 'oj_tag_%'";
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
        for (const auto& s : sqls) {
            mysql_real_query(lease.raw(), s, std::strlen(s));
        }
    }
    void ensure_admin() {
        auto lease = cli_->acquire();
        const std::string sql =
            "INSERT IGNORE INTO users (id,username,email,password_hash,is_admin) "
            "VALUES (1, 'oj_tag_admin', 'admin@tag.test', 'x', 1)";
        mysql_real_query(lease.raw(), sql.data(), static_cast<unsigned long>(sql.size()));
    }
    std::int64_t mkProblem(const std::string& t) {
        Problem p;
        p.title = t; p.content_md = "x"; p.difficulty = Difficulty::Easy; p.created_by = 1;
        return prob_->create(p).id;
    }

    std::shared_ptr<MysqlClient>    cli_;
    std::shared_ptr<ITagRepository> repo_;
    std::shared_ptr<MysqlProblemRepo> prob_;
};

TEST_F(TagRepoTest, ListAllReturnsAtLeast8) {
    auto tags = repo_->list_all();
    EXPECT_GE(tags.size(), 8u);
    for (std::size_t i = 1; i < tags.size(); ++i) {
        EXPECT_LT(tags[i-1].id, tags[i].id);  // id ASC
    }
}

TEST_F(TagRepoTest, FindByIdAndSlug) {
    auto by_id   = repo_->find_by_id(1);
    auto by_slug = repo_->find_by_slug("string");
    ASSERT_TRUE(by_id.has_value());
    ASSERT_TRUE(by_slug.has_value());
    EXPECT_EQ(by_slug->id, 2);
    EXPECT_EQ(by_id->name, "数组");
}

TEST_F(TagRepoTest, FindByIdReturnsNulloptForMissing) {
    EXPECT_FALSE(repo_->find_by_id(999999).has_value());
    EXPECT_FALSE(repo_->find_by_slug("nonexistent-slug-xyz").has_value());
}

TEST_F(TagRepoTest, FindByIdsRespectsInputOrderAndSkipsMissing) {
    auto tags = repo_->find_by_ids({3, 999, 1, 2});
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0].id, 3);
    EXPECT_EQ(tags[1].id, 1);
    EXPECT_EQ(tags[2].id, 2);
}

TEST_F(TagRepoTest, FindByIdsEmptyReturnsEmpty) {
    EXPECT_EQ(repo_->find_by_ids({}).size(), 0u);
}

TEST_F(TagRepoTest, SetProblemTagsAndTagsOfProblem) {
    auto pid = mkProblem("oj_tag_basic");
    repo_->set_problem_tags(pid, {1, 2, 5});
    auto tags = repo_->tags_of_problem(pid);
    ASSERT_EQ(tags.size(), 3u);
    EXPECT_EQ(tags[0].id, 1);
    EXPECT_EQ(tags[1].id, 2);
    EXPECT_EQ(tags[2].id, 5);
}

TEST_F(TagRepoTest, SetProblemTagsReplacesPreviousAssociations) {
    auto pid = mkProblem("oj_tag_replace");
    repo_->set_problem_tags(pid, {1, 2});
    EXPECT_EQ(repo_->tags_of_problem(pid).size(), 2u);
    repo_->set_problem_tags(pid, {5});
    auto after = repo_->tags_of_problem(pid);
    ASSERT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].id, 5);
}

TEST_F(TagRepoTest, SetProblemTagsEmptyClearsAll) {
    auto pid = mkProblem("oj_tag_clear");
    repo_->set_problem_tags(pid, {1, 2});
    EXPECT_EQ(repo_->tags_of_problem(pid).size(), 2u);
    repo_->set_problem_tags(pid, {});
    EXPECT_EQ(repo_->tags_of_problem(pid).size(), 0u);
}

TEST_F(TagRepoTest, TagIdsOfProblemMatchesTagsOfProblem) {
    auto pid = mkProblem("oj_tag_ids");
    repo_->set_problem_tags(pid, {1, 2, 3});
    auto ids = repo_->tag_ids_of_problem(pid);
    std::set<int> idset(ids.begin(), ids.end());
    EXPECT_EQ(idset.size(), 3u);
    EXPECT_TRUE(idset.count(1) == 1);
    EXPECT_TRUE(idset.count(2) == 1);
    EXPECT_TRUE(idset.count(3) == 1);
}

}  // namespace
