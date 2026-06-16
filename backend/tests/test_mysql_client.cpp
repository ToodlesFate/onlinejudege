// =============================================================================
//  test_mysql_client.cpp — MysqlClient 单元测试
//  覆盖：
//    - 构造 + connect 流程（错误 host / 错误端口 / 正确 host）
//    - 连接池：acquire() 阻塞、Lease RAII 自动归还、move 语义
//    - escape()：常规 / 引号 / 反斜杠 / NULL 字节 / UTF-8
//    - 线程安全：多线程并发 acquire/release 无 race
//
//  默认 SKIP（环境无 MySQL 时不阻塞 CI）；设置环境变量
//  OJ_RUN_MYSQL_TESTS=1 即可启用（与 test_user_repo_mysql.cpp 复用）。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "infra/mysql_client.hpp"

namespace {

using oj::common::MysqlConfig;
using oj::infra::MysqlClient;
using oj::infra::MysqlError;

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
    c.connect_timeout_sec = 3;
    return c;
}

class MysqlClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!mysql_tests_enabled()) {
            GTEST_SKIP() << "set OJ_RUN_MYSQL_TESTS=1 to run MySQL unit tests";
        }
    }
};

// ---------------------------------------------------------------------------
//  构造 + connect
// ---------------------------------------------------------------------------
TEST_F(MysqlClientTest, NotReadyBeforeConnect) {
    MysqlClient cli{make_cfg()};
    EXPECT_FALSE(cli.is_ready());
    EXPECT_EQ(cli.available(), 0u);
    EXPECT_EQ(cli.size(),       4u);  // pool_size 已设置
}

TEST_F(MysqlClientTest, ConnectEstablishesPool) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    EXPECT_TRUE(cli.is_ready());
    EXPECT_EQ(cli.available(), 4u);   // 全部空闲
}

TEST_F(MysqlClientTest, ConnectIsIdempotent) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    cli.connect();  // 第二次调用不应抛
    EXPECT_TRUE(cli.is_ready());
    EXPECT_EQ(cli.available(), 4u);
}

TEST_F(MysqlClientTest, ConnectFailsOnWrongHost) {
    MysqlConfig bad = make_cfg();
    bad.host = "127.0.0.255";      // 非路由地址
    bad.port = 1;                  // 端口 1 通常未监听
    bad.connect_timeout_sec = 2;   // 短超时让测试不挂死
    MysqlClient cli{bad};
    EXPECT_THROW(cli.connect(), MysqlError);
    EXPECT_FALSE(cli.is_ready());
}

TEST_F(MysqlClientTest, ConnectFailsOnBadCredentials) {
    MysqlConfig bad = make_cfg();
    bad.password = "definitely-wrong-password";
    bad.connect_timeout_sec = 2;
    MysqlClient cli{bad};
    EXPECT_THROW(cli.connect(), MysqlError);
    EXPECT_FALSE(cli.is_ready());
}

TEST_F(MysqlClientTest, ConnectFailsOnUnknownDatabase) {
    MysqlConfig bad = make_cfg();
    bad.database = "definitely_does_not_exist_xyz";
    bad.connect_timeout_sec = 2;
    MysqlClient cli{bad};
    EXPECT_THROW(cli.connect(), MysqlError);
    EXPECT_FALSE(cli.is_ready());
}

// ---------------------------------------------------------------------------
//  Lease RAII + 池语义
// ---------------------------------------------------------------------------
TEST_F(MysqlClientTest, LeaseAutoReturnOnDestruction) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    EXPECT_EQ(cli.available(), 4u);
    {
        auto lease = cli.acquire();
        EXPECT_TRUE(static_cast<bool>(lease));
        EXPECT_EQ(cli.available(), 3u);
    }
    EXPECT_EQ(cli.available(), 4u);  // 析构自动归还
}

TEST_F(MysqlClientTest, LeaseMoveTransfersOwnership) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    EXPECT_EQ(cli.available(), 4u);

    auto l1 = cli.acquire();
    EXPECT_EQ(cli.available(), 3u);

    auto l2 = std::move(l1);
    EXPECT_EQ(cli.available(), 3u);  // 移动不归还
    EXPECT_FALSE(static_cast<bool>(l1));
    EXPECT_TRUE(static_cast<bool>(l2));

    l2.release();
    EXPECT_EQ(cli.available(), 4u);
}

TEST_F(MysqlClientTest, LeaseExplicitReleaseIdempotent) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto lease = cli.acquire();
    lease.release();
    lease.release();  // 二次 release 不应崩
    EXPECT_EQ(cli.available(), 4u);
}

TEST_F(MysqlClientTest, PoolExhaustionBlocks) {
    MysqlConfig cfg = make_cfg();
    cfg.pool_size = 2;  // 限制小池便于测试
    MysqlClient cli{cfg};
    cli.connect();
    EXPECT_EQ(cli.available(), 2u);

    auto a = cli.acquire();
    auto b = cli.acquire();
    EXPECT_EQ(cli.available(), 0u);

    // 第三个 acquire 应该阻塞；用子线程验证
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        auto c = cli.acquire();
        acquired.store(true);
    });

    // 50ms 内不应获得
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(acquired.load());

    // 释放一个连接后 waiter 应能获得
    a.release();
    waiter.join();
    EXPECT_TRUE(acquired.load());
}

// ---------------------------------------------------------------------------
//  escape() —— SQL 注入防御
// ---------------------------------------------------------------------------
TEST_F(MysqlClientTest, EscapeReturnsValidStringForAsciiInput) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("hello world");
    // 不应包含未转义引号
    EXPECT_EQ(out, "hello world");
}

TEST_F(MysqlClientTest, EscapeHandlesSingleQuote) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("O'Brien");
    EXPECT_EQ(out, "O\\'Brien");
}

TEST_F(MysqlClientTest, EscapeHandlesDoubleQuote) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("say \"hi\"");
    EXPECT_EQ(out, "say \\\"hi\\\"");
}

TEST_F(MysqlClientTest, EscapeHandlesBackslash) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("path\\to\\file");
    EXPECT_EQ(out, "path\\\\to\\\\file");
}

TEST_F(MysqlClientTest, EscapeHandlesNewline) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("line1\nline2");
    EXPECT_EQ(out, "line1\\nline2");
}

TEST_F(MysqlClientTest, EscapeHandlesCarriageReturnAndNul) {
    // mysql_real_escape_string escapes: \0 \n \r \\ ' " \x1a
    // 注意：\b (backspace) 和 \t (tab) 不被转义（MySQL 直接接受）
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape(std::string("a\r\0b", 4));
    EXPECT_EQ(out, "a\\r\\0b");
}

TEST_F(MysqlClientTest, EscapeHandlesNullByte) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    std::string in("a\0b", 3);
    auto out = cli.escape(in);
    // mysql_real_escape_string 应当转义 \0 为 \0 (5 chars)
    EXPECT_EQ(out, "a\\0b");
}

TEST_F(MysqlClientTest, EscapeHandlesUtf8Bytes) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("中文");
    // mysql_real_escape_string 对 UTF-8 多字节序列不破坏（utf8mb4 字符集下）
    EXPECT_EQ(out, "中文");
    EXPECT_EQ(out.size(), 6u);  // 6 字节 UTF-8
}

TEST_F(MysqlClientTest, EscapeHandlesEmoji) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    auto out = cli.escape("🔒");
    EXPECT_EQ(out, "🔒");
    EXPECT_GT(out.size(), 0u);
}

TEST_F(MysqlClientTest, EscapeHandlesEmptyString) {
    MysqlClient cli{make_cfg()};
    cli.connect();
    EXPECT_EQ(cli.escape(""), "");
}

// ---------------------------------------------------------------------------
//  端到端：escape 嵌入 SQL 不破语法
// ---------------------------------------------------------------------------
TEST_F(MysqlClientTest, EscapedStringWorksInRealQuery) {
    MysqlClient cli{make_cfg()};
    cli.connect();

    // 清理可能存在的行
    {
        auto lease = cli.acquire();
        const std::string sql = "DELETE FROM users WHERE username IN ('esc_test_a','esc_test_b')";
        mysql_real_query(lease.raw(), sql.data(),
                         static_cast<unsigned long>(sql.size()));
    }

    // 插入两个含特殊字符的 username
    const std::string u1_safe = cli.escape("evil';--");
    const std::string u2_safe = cli.escape("path\\name\"");
    {
        auto lease = cli.acquire();
        const std::string sql1 = "INSERT INTO users (username,email,password_hash,is_admin) VALUES ('" +
                                 u1_safe + "', 'e1@x.com', 'h', 0)";
        const std::string sql2 = "INSERT INTO users (username,email,password_hash,is_admin) VALUES ('" +
                                 u2_safe + "', 'e2@x.com', 'h', 0)";
        ASSERT_EQ(mysql_real_query(lease.raw(), sql1.data(), sql1.size()), 0) << mysql_error(lease.raw());
        ASSERT_EQ(mysql_real_query(lease.raw(), sql2.data(), sql2.size()), 0) << mysql_error(lease.raw());
    }

    // 验证表还在 + 行数 = 2
    auto lease = cli.acquire();
    const std::string cnt = "SELECT COUNT(*) FROM users WHERE username IN ('evil\\';--', 'path\\\\name\"')";
    ASSERT_EQ(mysql_real_query(lease.raw(), cnt.data(),
                               static_cast<unsigned long>(cnt.size())), 0);
    MYSQL_RES* r = mysql_store_result(lease.raw());
    ASSERT_NE(r, nullptr);
    long long n = 0;
    if (auto row = mysql_fetch_row(r)) {
        if (row[0]) n = std::stoll(row[0]);
    }
    mysql_free_result(r);
    EXPECT_EQ(n, 2);

    // 清理
    const std::string cleanup = "DELETE FROM users WHERE email IN ('e1@x.com','e2@x.com')";
    mysql_real_query(lease.raw(), cleanup.data(),
                     static_cast<unsigned long>(cleanup.size()));
}

// ---------------------------------------------------------------------------
//  线程安全
// ---------------------------------------------------------------------------
TEST_F(MysqlClientTest, ConcurrentAcquireAndReleaseIsSafe) {
    MysqlConfig cfg = make_cfg();
    cfg.pool_size = 4;
    MysqlClient cli{cfg};
    cli.connect();

    constexpr int kThreads = 16;
    constexpr int kPerThread = 50;
    std::atomic<int> errors{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                auto lease = cli.acquire();
                // 模拟实际使用：跑一个简单查询；必须消费结果集，
                // 否则下一次同连接的查询会返回 "Commands out of sync"
                const std::string sql = "SELECT 1";
                if (mysql_real_query(lease.raw(), sql.data(), sql.size()) != 0) {
                    ++errors;
                    continue;
                }
                MYSQL_RES* r = mysql_store_result(lease.raw());
                if (r != nullptr) mysql_free_result(r);
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(cli.available(), 4u);  // 全部归还
}

TEST_F(MysqlClientTest, ConcurrentEscapeFromMultipleThreadsIsSafe) {
    MysqlClient cli{make_cfg()};
    cli.connect();

    constexpr int kThreads = 8;
    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 100; ++i) {
                try {
                    auto out = cli.escape("evil';DROP TABLE--");
                    // 验证：每个 `'` 前面必须有 `\`（转义标志）；
                    // 不能有"裸"的 `'`（SQL 注入风险）
                    bool ok = true;
                    for (std::size_t k = 0; k < out.size(); ++k) {
                        if (out[k] == '\'' && (k == 0 || out[k-1] != '\\')) {
                            ok = false;  // 裸单引号 = 没转义
                            break;
                        }
                    }
                    // 还要验证"反斜杠也必须转义"——如果原串无 `\`，输出也不应含奇数个连续 `\`
                    if (!ok) ++errors;
                } catch (...) {
                    ++errors;
                }
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(errors.load(), 0);
}

TEST_F(MysqlClientTest, AcquireFromMultipleThreadsReturnsDistinctConnections) {
    MysqlConfig cfg = make_cfg();
    cfg.pool_size = 3;
    MysqlClient cli{cfg};
    cli.connect();

    std::mutex mu;
    std::set<unsigned long> seen_thread_ids;
    std::vector<std::thread> ts;
    for (int t = 0; t < 3; ++t) {
        ts.emplace_back([&] {
            auto lease = cli.acquire();
            const auto tid = mysql_thread_id(lease.raw());
            std::lock_guard<std::mutex> lk(mu);
            seen_thread_ids.insert(tid);
        });
    }
    for (auto& th : ts) th.join();
    // 3 个并发线程应拿到 3 个不同的 MYSQL 连接
    EXPECT_EQ(seen_thread_ids.size(), 3u);
}

}  // namespace
