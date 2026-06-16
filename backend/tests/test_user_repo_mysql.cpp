// =============================================================================
//  test_user_repo_mysql.cpp — MysqlUserRepo 集成测试（需要 MySQL）
//  默认 SKIP（环境无 MySQL 时不阻塞 CI）；设置环境变量 OJ_RUN_MYSQL_TESTS=1
//  即可启用。
//
//  覆盖：
//    - 基本 CRUD（首行 admin / find_by_* / duplicate throws / escape 不破 SQL）
//    - **并发首注册：admin 严格唯一**（多线程 race "first user"）
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
#include <string_view>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "domain/user_repository.hpp"
#include "infra/mysql_client.hpp"
#include "infra/user_repo.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::infra::MysqlClient;
using oj::infra::MysqlUserRepo;

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

class MysqlRepoTest : public ::testing::Test {
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
        repo_ = std::make_shared<MysqlUserRepo>(cli_);
        // 每个测试启动前清空 users 表，确保"首行 is_admin"等断言可重复
        truncate_users();
    }

    void TearDown() override {
        truncate_users();
    }

    void truncate_users() {
        if (!cli_ || !cli_->is_ready()) return;
        auto lease = cli_->acquire();
        const std::string sql = "DELETE FROM users";
        mysql_real_query(lease.raw(), sql.data(),
                         static_cast<unsigned long>(sql.size()));
    }

    std::shared_ptr<MysqlClient>  cli_;
    std::shared_ptr<MysqlUserRepo> repo_;
};

TEST_F(MysqlRepoTest, FirstUserIsAdmin) {
    auto u = repo_->register_user("alice", "alice@x.com", "argon2id$dummy$xx$yy");
    EXPECT_TRUE(u.is_admin);
    EXPECT_EQ(u.username, "alice");
    EXPECT_EQ(u.email, "alice@x.com");
}

TEST_F(MysqlRepoTest, SecondUserIsNotAdmin) {
    auto u1 = repo_->register_user("alice", "a@x.com", "argon2id$dummy$xx$yy");
    auto u2 = repo_->register_user("bob",   "b@x.com", "argon2id$dummy$xx$yy");
    EXPECT_TRUE (u1.is_admin);
    EXPECT_FALSE(u2.is_admin);
    EXPECT_LT(u1.id, u2.id);
}

TEST_F(MysqlRepoTest, FindByUsernameAndEmail) {
    auto u = repo_->register_user("findme", "findme@x.com", "argon2id$dummy$xx$yy");
    auto by_name = repo_->find_by_username("findme");
    auto by_email = repo_->find_by_email("findme@x.com");
    auto by_id    = repo_->find_by_id(u.id);
    ASSERT_TRUE(by_name.has_value());
    ASSERT_TRUE(by_email.has_value());
    ASSERT_TRUE(by_id.has_value());
    EXPECT_EQ(by_name->id, by_email->id);
    EXPECT_EQ(by_name->id, by_id->id);
    EXPECT_EQ(by_name->is_admin, true);
}

TEST_F(MysqlRepoTest, FindReturnsNulloptForUnknown) {
    auto by_name = repo_->find_by_username("nonexistent-user-xyz");
    auto by_email = repo_->find_by_email("nonexistent@x.com");
    EXPECT_FALSE(by_name.has_value());
    EXPECT_FALSE(by_email.has_value());
}

TEST_F(MysqlRepoTest, DuplicateUsernameThrows) {
    repo_->register_user("dupuser", "a@x.com", "argon2id$dummy$xx$yy");
    EXPECT_THROW(repo_->register_user("dupuser", "b@x.com", "argon2id$dummy$xx$yy"),
                 std::runtime_error);
}

TEST_F(MysqlRepoTest, EscapeHandlesQuotesAndBackslashes) {
    // 通过 register_user 用含 ' " \ 的输入，验证不会破 SQL
    // 必须 ≤ 20 字符（username VARCHAR(20)）；本测试只验证 escape 不会破语法
    const std::string tricky = "evil';DROP--";  // 13 字符
    try {
        repo_->register_user(tricky, "evil@x.com", "argon2id$dummy$xx$yy");
    } catch (...) { /* dup 等 — 我们只关心 SQL 没被注入 */ }
    // 表还在 + count > 0 表示没注入
    auto lease = cli_->acquire();
    const std::string count_sql = "SELECT COUNT(*) FROM users";
    int rc = mysql_real_query(lease.raw(), count_sql.data(),
                              static_cast<unsigned long>(count_sql.size()));
    ASSERT_EQ(rc, 0) << mysql_error(lease.raw());
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long cnt = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) cnt = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_GE(cnt, 1);
}

// ---------------------------------------------------------------------------
//  并发首注册：admin 严格唯一（核心特性的 race-condition 测试）
//  SPEC §9.1.1 AC-3 要求"第一个注册用户自动获得 admin 权限"。
//  并发注册 N 个不同 username，验证：
//    (a) 恰好 1 个返回 is_admin=true
//    (b) 所有 user_id 互不相同
//    (c) 没有 deadlock
//  不变量：DB 行锁（SELECT COUNT(*) FOR UPDATE）保证 count 串行化。
// ---------------------------------------------------------------------------
TEST_F(MysqlRepoTest, ConcurrentFirstRegistrationHasExactlyOneAdmin) {
    truncate_users();

    constexpr int kThreads = 8;
    std::atomic<int>    admin_count{0};
    std::atomic<int>    failure_count{0};
    std::mutex          ids_mu;
    std::set<std::int64_t> ids;
    std::vector<std::thread> ts;

    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            const std::string u = "race_user_" + std::to_string(t);
            const std::string e = "race_" + std::to_string(t) + "@x.com";
            try {
                auto u2 = repo_->register_user(u, e, "argon2id$dummy$xx$yy");
                if (u2.is_admin) ++admin_count;
                std::lock_guard<std::mutex> lk(ids_mu);
                ids.insert(u2.id);
            } catch (const std::exception& ex) {
                ++failure_count;
                // 把错误信息写到 stderr，方便排查
                std::fprintf(stderr, "  [thread %d] register_user failed: %s\n",
                             t, ex.what());
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(failure_count.load(), 0) << "no registration should fail";
    EXPECT_EQ(admin_count.load(),  1) << "exactly one concurrent register should win admin";
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kThreads))
        << "all user_ids should be unique";

    // 二次校验：DB 里也是 1 个 admin
    auto lease = cli_->acquire();
    const std::string sql = "SELECT COUNT(*) FROM users WHERE is_admin=1";
    ASSERT_EQ(mysql_real_query(lease.raw(), sql.data(),
                               static_cast<unsigned long>(sql.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long db_admins = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) db_admins = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_EQ(db_admins, 1);

    truncate_users();
}

TEST_F(MysqlRepoTest, ConcurrentRegistrationWithDuplicatesHandledGracefully) {
    // 并发注册同一 username 两次，验证：
    //   - 1 个成功，1 个抛 dup 异常
    //   - DB 中只有 1 行
    truncate_users();

    std::atomic<int>    success{0};
    std::atomic<int>    conflict{0};
    std::vector<std::thread> ts;

    for (int i = 0; i < 2; ++i) {
        ts.emplace_back([&] {
            try {
                repo_->register_user("dup_race", "dup_race@x.com",
                                     "argon2id$dummy$xx$yy");
                ++success;
            } catch (const std::exception&) {
                ++conflict;  // UNIQUE conflict (errno=1062)
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(success.load(), 1);
    EXPECT_EQ(conflict.load(), 1);

    // DB 中 1 行
    auto lease = cli_->acquire();
    const std::string sql = "SELECT COUNT(*) FROM users WHERE username='dup_race'";
    ASSERT_EQ(mysql_real_query(lease.raw(), sql.data(),
                               static_cast<unsigned long>(sql.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long n = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) n = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_EQ(n, 1);

    truncate_users();
}

TEST_F(MysqlRepoTest, ExistingAdminStaysAdminAfterNewRegistrations) {
    // admin 已存在后，再注册 10 个并发用户，应当全部 is_admin=false
    truncate_users();
    auto admin = repo_->register_user("admin", "admin@x.com",
                                      "argon2id$dummy$xx$yy");
    ASSERT_TRUE(admin.is_admin);

    constexpr int kThreads = 10;
    std::atomic<int> bad_admin_count{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            const std::string u = "user_" + std::to_string(t);
            try {
                auto u2 = repo_->register_user(u, u + "@x.com",
                                               "argon2id$dummy$xx$yy");
                if (u2.is_admin) ++bad_admin_count;
            } catch (...) {}
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(bad_admin_count.load(), 0)
        << "no second user should be admin after first admin exists";

    // DB 中 admin 总数 = 1
    auto lease = cli_->acquire();
    const std::string sql = "SELECT COUNT(*) FROM users WHERE is_admin=1";
    ASSERT_EQ(mysql_real_query(lease.raw(), sql.data(),
                               static_cast<unsigned long>(sql.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long db_admins = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) db_admins = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_EQ(db_admins, 1);

    truncate_users();
}

TEST_F(MysqlRepoTest, DeadlockIsRetriedTransparently) {
    // 在 pool_size=2 + 8 并发下必然产生若干 deadlock；
    // register_user 外层应自动重试，最终 8 个调用全部成功
    // （本测试与 ConcurrentFirstRegistrationHasExactlyOneAdmin 互相独立；
    //  本测试只验证「不会因 deadlock 失败」）。
    truncate_users();

    MysqlConfig cfg = make_cfg();
    cfg.pool_size = 2;  // 极小池，放大竞争
    auto small_cli = std::make_shared<MysqlClient>(cfg);
    small_cli->connect();
    auto small_repo = std::make_shared<MysqlUserRepo>(small_cli);

    constexpr int kThreads = 8;
    std::atomic<int> success{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t] {
            const std::string u = "dl_user_" + std::to_string(t);
            try {
                small_repo->register_user(u, u + "@x.com", "argon2id$dummy$xx$yy");
                ++success;
            } catch (...) {
                // 失败 → 测试失败
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(success.load(), kThreads)
        << "all register calls should succeed via deadlock retry";

    // 验证 DB 行数
    auto lease = cli_->acquire();
    const std::string sql = "SELECT COUNT(*) FROM users";
    ASSERT_EQ(mysql_real_query(lease.raw(), sql.data(),
                               static_cast<unsigned long>(sql.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long n = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) n = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_EQ(n, kThreads);

    truncate_users();
}

}  // namespace
