#include "infra/user_repo.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <mysql.h>
#include <spdlog/spdlog.h>

#include "domain/user_repository.hpp"

namespace oj::infra {

namespace {

// 错误码
constexpr unsigned int kErDupEntry   = 1062;  // ER_DUP_ENTRY
constexpr unsigned int kErDeadlock   = 1213;  // ER_LOCK_DEADLOCK
constexpr unsigned int kErLockWait   = 1205;  // ER_LOCK_WAIT_TIMEOUT
// InnoDB 死锁重试参数：3 次，间隔 5ms / 15ms（带 jitter）
constexpr int          kDeadlockMaxRetries = 3;

// 拼接 "SELECT ... WHERE username='xxx'" 中字面量。
// 长度 = N*2+1 是 mysql_real_escape_string 的最大输出长度上界。
std::string quote(MysqlClient& cli, std::string_view s) {
    return "'" + cli.escape(s) + "'";
}

// 抛带 errno + sqlstate 的异常
[[noreturn]] void throw_stmt(MYSQL* m, const char* what) {
    const char* err = mysql_error(m);
    const char* sqlstate = mysql_sqlstate(m);
    throw std::runtime_error(std::string{"MysqlUserRepo: "} + what + ": [" +
                             sqlstate + "] " + err);
}

// 把 MYSQL_BIND-free 的 mysql_real_query + 手动取列的简单 SELECT 转成 User。
// 适用于只查 (id, username, email, password_hash, is_admin) 的小结果集。
std::optional<oj::domain::User> fetch_user(MYSQL* m, const std::string& sql) {
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, "fetch_user: query");
    }
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) == 0) {
            return std::nullopt;  // 0 行结果
        }
        throw_stmt(m, "fetch_user: store_result");
    }
    std::optional<oj::domain::User> out;
    if (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::User u;
        u.id           = row[0] ? std::stoll(row[0]) : 0;
        u.username     = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        u.email        = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        u.password_hash= (row[3] && lens[3]) ? std::string(row[3], lens[3]) : std::string{};
        u.is_admin     = (row[4] && row[4][0] != '0');
        out = std::move(u);
    }
    mysql_free_result(res);
    return out;
}

}  // namespace

oj::domain::User MysqlUserRepo::register_user(std::string_view username,
                                              std::string_view email,
                                              std::string_view password_hash) {
    // 重试外层：捕获 InnoDB deadlock (1213) / lock-wait timeout (1205) 时
    // 回滚并重试。这是 MySQL 官方对瞬态死锁的推荐做法。
    // 重试策略：3 次，间隔 5ms × attempt（jitter 简单用线性退避）。
    for (int attempt = 0; ; ++attempt) {
        try {
            return register_user_once(username, email, password_hash);
        } catch (const std::runtime_error& e) {
            const std::string what = e.what();
            const bool is_deadlock =
                what.find(std::to_string(kErDeadlock)) != std::string::npos ||
                what.find("Deadlock")                  != std::string::npos ||
                what.find(std::to_string(kErLockWait)) != std::string::npos;
            if (!is_deadlock || attempt + 1 >= kDeadlockMaxRetries) {
                throw;
            }
            spdlog::warn("MysqlUserRepo::register_user deadlock (attempt {}/{}); retrying",
                         attempt + 1, kDeadlockMaxRetries);
            // 用 RAII 锁住 lease 等待连接重置；然后给 MySQL 一小段时间恢复
            std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
        }
    }
}

oj::domain::User MysqlUserRepo::register_user_once(std::string_view username,
                                                  std::string_view email,
                                                  std::string_view password_hash) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    // 用 std::string 持有 SQL，避免 mysql_real_query 把末尾 '\0' 当成 SQL 一部分
    // （"near ''" 错误就是 17/41/8/6 这种硬编码长度偶尔写错时的常见症状）。
    const std::string kStartTxn   = "START TRANSACTION";
    const std::string kCount      = "SELECT COUNT(*) FROM users FOR UPDATE";
    const std::string kCommit     = "COMMIT";
    const std::string kRollback   = "ROLLBACK";

    // 1) 显式开启事务；FOR UPDATE 锁表避免并发双 admin
    if (mysql_real_query(m, kStartTxn.data(), kStartTxn.size()) != 0) {
        throw_stmt(m, "register_user: START TRANSACTION");
    }

    bool committed = false;
    struct RollbackGuard {
        MYSQL* m;
        const std::string* rollback_sql;
        bool*  flag;
        ~RollbackGuard() {
            if (!*flag) {
                mysql_real_query(m, rollback_sql->data(),
                                 static_cast<unsigned long>(rollback_sql->size()));
            }
        }
    } guard{m, &kRollback, &committed};

    // 2) count(*) + 行锁（即使我们用的是 AUTO_INCREMENT 主键，FOR UPDATE 仍然
    //    锁住整个表扫描过程）
    if (mysql_real_query(m, kCount.data(), kCount.size()) != 0) {
        throw_stmt(m, "register_user: COUNT");
    }
    MYSQL_RES* cres = mysql_store_result(m);
    if (cres == nullptr) {
        throw_stmt(m, "register_user: store COUNT");
    }
    long long existing = 0;
    if (MYSQL_ROW r = mysql_fetch_row(cres)) {
        if (r[0]) existing = std::stoll(r[0]);
    }
    mysql_free_result(cres);

    const bool make_admin = (existing == 0);

    // 3) INSERT
    const std::string q_user     = quote(*mysql_, username);
    const std::string q_email    = quote(*mysql_, email);
    const std::string q_hash     = quote(*mysql_, password_hash);
    const std::string sql =
        "INSERT INTO users (username, email, password_hash, is_admin) VALUES (" +
        q_user + "," + q_email + "," + q_hash + "," +
        (make_admin ? "1" : "0") + ")";

    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        const unsigned int errno_ = mysql_errno(m);
        const char* err = mysql_error(m);
        if (errno_ == kErDupEntry) {
            // 透传 errno，让调用方按 errno 区分是 username 还是 email 冲突
            throw std::runtime_error(std::string{"MysqlUserRepo: "} + err +
                                     " [errno=" + std::to_string(errno_) + "]");
        }
        throw_stmt(m, "register_user: INSERT");
    }
    const std::int64_t new_id = static_cast<std::int64_t>(mysql_insert_id(m));

    // 4) COMMIT
    if (mysql_real_query(m, kCommit.data(), kCommit.size()) != 0) {
        throw_stmt(m, "register_user: COMMIT");
    }
    committed = true;

    spdlog::info("MysqlUserRepo::register_user id={} username='{}' is_admin={}",
                 new_id, std::string{username}, make_admin);

    oj::domain::User u;
    u.id            = new_id;
    u.username      = std::string{username};
    u.email         = std::string{email};
    u.password_hash = std::string{password_hash};
    u.is_admin      = make_admin;
    return u;
}

std::optional<oj::domain::User>
MysqlUserRepo::find_by_username(std::string_view username) {
    auto lease = mysql_->acquire();
    const std::string sql =
        "SELECT id, username, email, password_hash, is_admin FROM users WHERE username=" +
        quote(*mysql_, username) + " LIMIT 1";
    return fetch_user(lease.raw(), sql);
}

std::optional<oj::domain::User>
MysqlUserRepo::find_by_email(std::string_view email) {
    auto lease = mysql_->acquire();
    const std::string sql =
        "SELECT id, username, email, password_hash, is_admin FROM users WHERE email=" +
        quote(*mysql_, email) + " LIMIT 1";
    return fetch_user(lease.raw(), sql);
}

std::optional<oj::domain::User>
MysqlUserRepo::find_by_id(std::int64_t id) {
    auto lease = mysql_->acquire();
    const std::string sql =
        "SELECT id, username, email, password_hash, is_admin FROM users WHERE id=" +
        std::to_string(id) + " LIMIT 1";
    return fetch_user(lease.raw(), sql);
}

}  // namespace oj::infra
