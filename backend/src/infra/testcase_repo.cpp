// =============================================================================
//  MysqlTestcaseRepo 实现
// =============================================================================

#include "infra/testcase_repo.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mysql.h>
#include <spdlog/spdlog.h>

#include "domain/problem_types.hpp"

namespace oj::infra {

namespace {

std::string quote(MysqlClient& cli, std::string_view s) {
    return "'" + cli.escape(s) + "'";
}

[[noreturn]] void throw_stmt(MYSQL* m, const char* what) {
    const char* err      = mysql_error(m);
    const char* sqlstate = mysql_sqlstate(m);
    throw std::runtime_error(std::string{"MysqlTestcaseRepo: "} + what + ": [" +
                             sqlstate + "] " + err);
}

void exec_simple(MYSQL* m, const std::string& sql, const char* what) {
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, what);
    }
}

std::int64_t parse_int64(const char* raw) {
    return raw ? std::stoll(raw) : 0;
}
int parse_int(const char* raw) {
    return raw ? std::stoi(raw) : 0;
}
bool parse_bool(const char* raw) {
    return raw && raw[0] != '0' && raw[0] != '\0';
}

// 取列：id, problem_id, case_index, input, expected_output, is_sample, score
std::vector<oj::domain::Testcase>
fetch_testcases(MYSQL* m, const std::string& sql) {
    exec_simple(m, sql, "fetch: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) == 0) return {};
        throw_stmt(m, "fetch: store_result");
    }
    std::vector<oj::domain::Testcase> out;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Testcase t;
        t.id               = parse_int64(row[0]);
        t.problem_id       = parse_int64(row[1]);
        t.case_index       = parse_int(row[2]);
        t.input            = (row[3] && lens[3]) ? std::string(row[3], lens[3]) : std::string{};
        t.expected_output  = (row[4] && lens[4]) ? std::string(row[4], lens[4]) : std::string{};
        t.is_sample        = parse_bool(row[5]);
        t.score            = parse_int(row[6]);
        out.push_back(std::move(t));
    }
    mysql_free_result(res);
    return out;
}

}  // namespace

std::vector<oj::domain::Testcase>
MysqlTestcaseRepo::list_by_problem(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql =
        "SELECT id, problem_id, case_index, input, expected_output, is_sample, score "
        "FROM testcases WHERE problem_id=" + std::to_string(problem_id) +
        " ORDER BY case_index";
    return fetch_testcases(m, sql);
}

std::vector<oj::domain::Testcase>
MysqlTestcaseRepo::list_samples(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql =
        "SELECT id, problem_id, case_index, input, expected_output, is_sample, score "
        "FROM testcases WHERE problem_id=" + std::to_string(problem_id) +
        " AND is_sample=1 ORDER BY case_index";
    return fetch_testcases(m, sql);
}

void MysqlTestcaseRepo::create_many(std::int64_t problem_id,
                                    const std::vector<oj::domain::Testcase>& cases) {
    if (cases.empty()) return;
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string kStartTxn = "START TRANSACTION";
    const std::string kCommit   = "COMMIT";
    const std::string kRollback = "ROLLBACK";
    exec_simple(m, kStartTxn, "create_many: START TRANSACTION");
    bool committed = false;
    struct RollbackGuard {
        MYSQL* m;
        const std::string* rollback_sql;
        bool* flag;
        ~RollbackGuard() { if (!*flag) { mysql_real_query(m, rollback_sql->data(), static_cast<unsigned long>(rollback_sql->size())); } }
    } guard{m, &kRollback, &committed};

    for (const auto& t : cases) {
        if (t.case_index < 1) {
            throw std::runtime_error("MysqlTestcaseRepo::create_many: case_index must be >= 1");
        }
        const std::string sql =
            "INSERT INTO testcases "
            "(problem_id, case_index, input, expected_output, is_sample, score) VALUES (" +
            std::to_string(problem_id) + "," +
            std::to_string(t.case_index) + "," +
            quote(*mysql_, t.input) + "," +
            quote(*mysql_, t.expected_output) + "," +
            (t.is_sample ? "1" : "0") + "," +
            std::to_string(t.score) + ")";
        if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
            throw_stmt(m, "create_many: INSERT");
        }
    }
    exec_simple(m, kCommit, "create_many: COMMIT");
    committed = true;
    spdlog::info("MysqlTestcaseRepo::create_many problem_id={} n={}", problem_id, cases.size());
}

void MysqlTestcaseRepo::replace_by_problem(std::int64_t problem_id,
                                           const std::vector<oj::domain::Testcase>& cases) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string kStartTxn = "START TRANSACTION";
    const std::string kCommit   = "COMMIT";
    const std::string kRollback = "ROLLBACK";
    exec_simple(m, kStartTxn, "replace: START TRANSACTION");
    bool committed = false;
    struct RollbackGuard {
        MYSQL* m;
        const std::string* rollback_sql;
        bool* flag;
        ~RollbackGuard() { if (!*flag) { mysql_real_query(m, rollback_sql->data(), static_cast<unsigned long>(rollback_sql->size())); } }
    } guard{m, &kRollback, &committed};

    // 1) DELETE 全部
    const std::string del_sql = "DELETE FROM testcases WHERE problem_id=" +
                                 std::to_string(problem_id);
    exec_simple(m, del_sql, "replace: DELETE");

    // 2) INSERT 新集合
    for (const auto& t : cases) {
        if (t.case_index < 1) {
            throw std::runtime_error("MysqlTestcaseRepo::replace: case_index must be >= 1");
        }
        const std::string sql =
            "INSERT INTO testcases "
            "(problem_id, case_index, input, expected_output, is_sample, score) VALUES (" +
            std::to_string(problem_id) + "," +
            std::to_string(t.case_index) + "," +
            quote(*mysql_, t.input) + "," +
            quote(*mysql_, t.expected_output) + "," +
            (t.is_sample ? "1" : "0") + "," +
            std::to_string(t.score) + ")";
        if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
            throw_stmt(m, "replace: INSERT");
        }
    }
    exec_simple(m, kCommit, "replace: COMMIT");
    committed = true;
    spdlog::info("MysqlTestcaseRepo::replace problem_id={} n={}", problem_id, cases.size());
}

void MysqlTestcaseRepo::delete_by_problem(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql = "DELETE FROM testcases WHERE problem_id=" +
                            std::to_string(problem_id);
    exec_simple(m, sql, "delete_by_problem: DELETE");
    spdlog::info("MysqlTestcaseRepo::delete_by_problem problem_id={}", problem_id);
}

}  // namespace oj::infra
