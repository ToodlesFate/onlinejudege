// =============================================================================
//  MysqlSubmissionRepo 实现 —— SPEC §3.2.2 / §4.2 submissions / submission_cases
//
//  风格与 MysqlUserRepo 完全一致：
//    1) escape + quote 拼 SQL
//    2) START TRANSACTION / COMMIT / ROLLBACK with RollbackGuard
//    3) escape 通过 MysqlClient::escape (mysql_real_escape_string)
//    4) InnoDB deadlock (1213) / lock-wait (1205) 自动重试 3 次
//
//  关键 SQL：
//    claim_one:
//      START TRANSACTION;
//      SELECT id, user_id, problem_id, language, code, created_at
//        FROM submissions WHERE status='queued' ORDER BY created_at
//        LIMIT 1 FOR UPDATE SKIP LOCKED;
//      -- 如果命中 → UPDATE 该行 status='running'，COMMIT
//      -- 如果 0 行   → ROLLBACK + return false
// =============================================================================

#include "infra/submission_repo.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <mysql.h>
#include <spdlog/spdlog.h>

#include "domain/submission_types.hpp"

namespace oj::infra {

namespace {

// 错误码
constexpr unsigned int kErDeadlock  = 1213;  // ER_LOCK_DEADLOCK
constexpr unsigned int kErLockWait  = 1205;  // ER_LOCK_WAIT_TIMEOUT
constexpr int          kDeadlockMaxRetries = 3;

std::string quote(MysqlClient& cli, std::string_view s) {
    return "'" + cli.escape(s) + "'";
}

[[noreturn]] void throw_stmt(MYSQL* m, const char* what) {
    const char* err      = mysql_error(m);
    const char* sqlstate = mysql_sqlstate(m);
    throw std::runtime_error(std::string{"MysqlSubmissionRepo: "} + what +
                             ": [" + sqlstate + "] " + err);
}

void exec_simple(MYSQL* m, const std::string& sql, const char* what) {
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, what);
    }
}

// "YYYY-MM-DD HH:MM:SS" → ISO 8601 "YYYY-MM-DDTHH:MM:SSZ"
std::string datetime_to_iso8601(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    auto sp = s.find(' ');
    if (sp != std::string::npos) s[sp] = 'T';
    s.push_back('Z');
    return s;
}

std::int64_t parse_int64(const char* raw) { return raw ? std::stoll(raw) : 0; }
int          parse_int  (const char* raw) { return raw ? std::stoi(raw)  : 0; }
bool         parse_bool (const char* raw) { return raw && raw[0] != '0' && raw[0] != '\0'; }

// 简单的"是否是 deadlock / lock-wait"判定，按 mysql_errno
bool is_deadlock_or_lock_wait(unsigned int errno_) noexcept {
    return errno_ == kErDeadlock || errno_ == kErLockWait;
}

}  // namespace

// ---------------------------------------------------------------------------
//  create
// ---------------------------------------------------------------------------
std::int64_t MysqlSubmissionRepo::create(std::int64_t user_id,
                                         std::int64_t problem_id,
                                         oj::domain::Language language,
                                         std::string_view code) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        "INSERT INTO submissions (user_id, problem_id, language, code, status) "
        "VALUES (" +
        std::to_string(user_id) + "," +
        std::to_string(problem_id) + "," +
        quote(*mysql_, oj::domain::to_string(language)) + "," +
        quote(*mysql_, code) + "," +
        "'queued')";
    exec_simple(m, sql, "create: INSERT");
    const std::int64_t new_id = static_cast<std::int64_t>(mysql_insert_id(m));
    spdlog::info("MysqlSubmissionRepo::create id={} user_id={} problem_id={} lang={}",
                 new_id, user_id, problem_id, oj::domain::to_string(language));
    return new_id;
}

// ---------------------------------------------------------------------------
//  claim_one —— SELECT FOR UPDATE SKIP LOCKED + UPDATE
//  注意：本方法不开外层事务锁整张表；而是用一个 transaction 把
//  SELECT + UPDATE 放在一起，行锁会被其他 worker 跳过（SKIP LOCKED）。
// ---------------------------------------------------------------------------
bool MysqlSubmissionRepo::claim_one(oj::domain::ClaimedTask& out) {
    for (int attempt = 0; ; ++attempt) {
        try {
            auto lease = mysql_->acquire();
            MYSQL* m = lease.raw();

            const std::string kStartTxn = "START TRANSACTION";
            const std::string kCommit   = "COMMIT";
            const std::string kRollback = "ROLLBACK";

            exec_simple(m, kStartTxn, "claim_one: START TRANSACTION");
            bool committed = false;
            struct RollbackGuard {
                MYSQL* m;
                const std::string* rollback_sql;
                bool* flag;
                ~RollbackGuard() {
                    if (!*flag) {
                        mysql_real_query(m, rollback_sql->data(),
                                         static_cast<unsigned long>(rollback_sql->size()));
                    }
                }
            } guard{m, &kRollback, &committed};

            // SELECT FOR UPDATE SKIP LOCKED —— MySQL 8.0+ 特性
            const std::string select_sql =
                "SELECT id, problem_id, language, code, created_at "
                "FROM submissions WHERE status='queued' ORDER BY created_at "
                "LIMIT 1 FOR UPDATE SKIP LOCKED";
            exec_simple(m, select_sql, "claim_one: SELECT");
            MYSQL_RES* res = mysql_store_result(m);
            if (res == nullptr) {
                if (mysql_field_count(m) == 0) {
                    // 0 行；正常 return false
                    mysql_real_query(m, kRollback.data(),
                                     static_cast<unsigned long>(kRollback.size()));
                    return false;
                }
                throw_stmt(m, "claim_one: store_result");
            }
            MYSQL_ROW row = mysql_fetch_row(res);
            if (row == nullptr) {
                mysql_free_result(res);
                mysql_real_query(m, kRollback.data(),
                                 static_cast<unsigned long>(kRollback.size()));
                return false;
            }
            unsigned long* lens = mysql_fetch_lengths(res);
            oj::domain::ClaimedTask t;
            t.submission_id = parse_int64(row[0]);
            t.problem_id    = parse_int64(row[1]);
            std::string lang_str = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
            auto lang_opt = oj::domain::language_from_string(lang_str);
            t.language      = lang_opt.value_or(oj::domain::Language::Cpp);
            t.code          = (row[3] && lens[3]) ? std::string(row[3], lens[3]) : std::string{};
            t.created_at    = datetime_to_iso8601(row[4]);
            mysql_free_result(res);

            // UPDATE → status='running'
            const std::string update_sql =
                "UPDATE submissions SET status='running' WHERE id=" +
                std::to_string(t.submission_id);
            exec_simple(m, update_sql, "claim_one: UPDATE");
            if (mysql_affected_rows(m) == 0) {
                // 极端情况：行已经被其他路径改了；当作没抢到
                mysql_real_query(m, kRollback.data(),
                                 static_cast<unsigned long>(kRollback.size()));
                return false;
            }

            exec_simple(m, kCommit, "claim_one: COMMIT");
            committed = true;

            spdlog::info("MysqlSubmissionRepo::claim_one id={} problem_id={}",
                         t.submission_id, t.problem_id);
            out = std::move(t);
            return true;
        } catch (const std::runtime_error& e) {
            const unsigned int errno_ = mysql_errno(mysql_->acquire().raw());
            // 注意：上面 mysql_->acquire() 是临时 lease，不会真的占住锁；
            // 仅为读 errno。errno_ 可能是 0（已经被前面 throw 覆盖过），
            // 这里用错误信息关键字兜底。
            std::string what = e.what();
            const bool is_deadlock =
                is_deadlock_or_lock_wait(errno_) ||
                what.find("Deadlock") != std::string::npos ||
                what.find("Lock wait") != std::string::npos ||
                what.find(std::to_string(kErDeadlock)) != std::string::npos ||
                what.find(std::to_string(kErLockWait)) != std::string::npos;
            if (!is_deadlock || attempt + 1 >= kDeadlockMaxRetries) {
                throw;
            }
            spdlog::warn("MysqlSubmissionRepo::claim_one deadlock (attempt {}/{}); retrying",
                         attempt + 1, kDeadlockMaxRetries);
            std::this_thread::sleep_for(std::chrono::milliseconds(5 * (attempt + 1)));
        }
    }
}

// ---------------------------------------------------------------------------
//  load_task —— 拉完整负载：submission + problem + testcases
// ---------------------------------------------------------------------------
oj::domain::JudgeTaskPayload
MysqlSubmissionRepo::load_task(std::int64_t submission_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    oj::domain::JudgeTaskPayload t;
    t.submission_id = submission_id;

    // 1) submissions 行
    {
        const std::string sql =
            "SELECT problem_id, language, code FROM submissions WHERE id=" +
            std::to_string(submission_id) + " LIMIT 1";
        exec_simple(m, sql, "load_task: submission SELECT");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "load_task: submission store_result");
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row == nullptr) {
            mysql_free_result(res);
            throw std::runtime_error("MysqlSubmissionRepo::load_task: submission " +
                                     std::to_string(submission_id) + " not found");
        }
        unsigned long* lens = mysql_fetch_lengths(res);
        t.problem_id = parse_int64(row[0]);
        std::string lang_str = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        t.language   = oj::domain::language_from_string(lang_str).value_or(oj::domain::Language::Cpp);
        t.code       = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        mysql_free_result(res);
    }

    // 2) problems 行（取 limits）
    {
        const std::string sql =
            "SELECT time_limit_ms, memory_limit_mb, output_limit_mb "
            "FROM problems WHERE id=" + std::to_string(t.problem_id) + " LIMIT 1";
        exec_simple(m, sql, "load_task: problem SELECT");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "load_task: problem store_result");
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row == nullptr) {
            mysql_free_result(res);
            throw std::runtime_error("MysqlSubmissionRepo::load_task: problem " +
                                     std::to_string(t.problem_id) + " not found");
        }
        t.time_limit_ms   = parse_int(row[0]);
        t.memory_limit_mb = parse_int(row[1]);
        t.output_limit_mb = parse_int(row[2]);
        mysql_free_result(res);
    }

    // 3) testcases 行（input + expected_output + case_index）
    {
        const std::string sql =
            "SELECT case_index, input, expected_output FROM testcases "
            "WHERE problem_id=" + std::to_string(t.problem_id) +
            " ORDER BY case_index";
        exec_simple(m, sql, "load_task: testcases SELECT");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "load_task: testcases store_result");
        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            unsigned long* lens = mysql_fetch_lengths(res);
            std::string in  = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
            std::string out = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
            t.testcases.emplace_back(std::move(in), std::move(out));
        }
        mysql_free_result(res);
    }

    return t;
}

// ---------------------------------------------------------------------------
//  update_status
// ---------------------------------------------------------------------------
void MysqlSubmissionRepo::update_status(std::int64_t submission_id,
                                        oj::domain::SubmissionStatus new_status) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    std::string sql =
        "UPDATE submissions SET status=" + quote(*mysql_, oj::domain::to_string(new_status));
    if (new_status == oj::domain::SubmissionStatus::Finished) {
        sql += ", finished_at=NOW()";
    }
    sql += " WHERE id=" + std::to_string(submission_id);
    exec_simple(m, sql, "update_status: UPDATE");
    if (mysql_affected_rows(m) == 0) {
        throw std::runtime_error("MysqlSubmissionRepo::update_status: id not found: " +
                                 std::to_string(submission_id));
    }
    spdlog::info("MysqlSubmissionRepo::update_status id={} status={}",
                 submission_id, oj::domain::to_string(new_status));
}

// ---------------------------------------------------------------------------
//  finish
// ---------------------------------------------------------------------------
void MysqlSubmissionRepo::finish(std::int64_t submission_id,
                                 oj::domain::SubmissionResult result,
                                 int total_score,
                                 int time_used_ms,
                                 int memory_used_kb,
                                 std::string_view compile_output,
                                 std::string_view judge_message) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    // judge_message 截断到 500 字符（与 submissions.judge_message VARCHAR(500) 对齐）
    std::string msg(judge_message);
    if (msg.size() > 500) msg.resize(500);

    const std::string sql =
        "UPDATE submissions SET "
        "status='finished',"
        "result="         + quote(*mysql_, oj::domain::to_string(result)) + ","
        "total_score="    + std::to_string(total_score) + ","
        "time_used_ms="   + std::to_string(time_used_ms) + ","
        "memory_used_kb=" + std::to_string(memory_used_kb) + ","
        "compile_output=" + quote(*mysql_, compile_output) + ","
        "judge_message="  + quote(*mysql_, msg) + ","
        "finished_at=NOW() "
        "WHERE id=" + std::to_string(submission_id);

    exec_simple(m, sql, "finish: UPDATE");
    if (mysql_affected_rows(m) == 0) {
        throw std::runtime_error("MysqlSubmissionRepo::finish: id not found: " +
                                 std::to_string(submission_id));
    }
    spdlog::info("MysqlSubmissionRepo::finish id={} result={} score={}",
                 submission_id, oj::domain::to_string(result), total_score);
}

// ---------------------------------------------------------------------------
//  insert_case
// ---------------------------------------------------------------------------
void MysqlSubmissionRepo::insert_case(std::int64_t submission_id,
                                      const oj::domain::SubmissionCase& c) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    // 持久化策略：仅 is_sample=1 时存 user_output；其它置 NULL
    std::string user_output_sql;
    if (c.is_sample) {
        user_output_sql = quote(*mysql_, c.user_output);
    } else {
        user_output_sql = "NULL";
    }

    const std::string sql =
        "INSERT INTO submission_cases "
        "(submission_id, case_index, status, time_used_ms, memory_used_kb, "
        " score, is_sample, user_output) VALUES (" +
        std::to_string(submission_id) + "," +
        std::to_string(c.case_index) + "," +
        quote(*mysql_, oj::domain::to_string(c.status)) + "," +
        std::to_string(c.time_used_ms) + "," +
        std::to_string(c.memory_used_kb) + "," +
        std::to_string(c.score) + "," +
        (c.is_sample ? "1" : "0") + "," +
        user_output_sql + ")";
    exec_simple(m, sql, "insert_case: INSERT");
}

// ---------------------------------------------------------------------------
//  mark_all_running_as_se_on_shutdown
// ---------------------------------------------------------------------------
void MysqlSubmissionRepo::mark_all_running_as_se_on_shutdown(std::string_view reason) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    std::string msg(reason);
    if (msg.size() > 500) msg.resize(500);

    const std::string sql =
        "UPDATE submissions SET status='finished', result='SE', "
        "judge_message=" + quote(*mysql_, msg) + ", finished_at=NOW() "
        "WHERE status='running'";
    exec_simple(m, sql, "mark_all_running_as_se_on_shutdown: UPDATE");
    const auto affected = mysql_affected_rows(m);
    spdlog::warn("MysqlSubmissionRepo::mark_all_running_as_se_on_shutdown: {} submission(s) marked SE",
                 static_cast<long long>(affected));
}

// ---------------------------------------------------------------------------
//  find_by_id
// ---------------------------------------------------------------------------
std::optional<oj::domain::Submission>
MysqlSubmissionRepo::find_by_id(std::int64_t id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        "SELECT id, user_id, problem_id, language, code, status, result, "
        "total_score, time_used_ms, memory_used_kb, compile_output, judge_message, "
        "created_at, finished_at FROM submissions WHERE id=" +
        std::to_string(id) + " LIMIT 1";
    exec_simple(m, sql, "find_by_id: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) == 0) return std::nullopt;
        throw_stmt(m, "find_by_id: store_result");
    }
    std::optional<oj::domain::Submission> out;
    if (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Submission s;
        s.id              = parse_int64(row[0]);
        s.user_id         = parse_int64(row[1]);
        s.problem_id      = parse_int64(row[2]);
        auto lang_opt = oj::domain::language_from_string(
            row[3] ? std::string_view(row[3], lens[3]) : std::string_view{});
        s.language        = lang_opt.value_or(oj::domain::Language::Cpp);
        s.code            = (row[4] && lens[4]) ? std::string(row[4], lens[4]) : std::string{};
        auto stat_opt = oj::domain::submission_status_from_string(
            row[5] ? std::string_view(row[5], lens[5]) : std::string_view{});
        s.status          = stat_opt.value_or(oj::domain::SubmissionStatus::Queued);
        if (row[6] && lens[6] > 0) {
            auto r_opt = oj::domain::submission_result_from_string(
                std::string_view(row[6], lens[6]));
            if (r_opt.has_value()) s.result = *r_opt;
        }
        s.total_score     = parse_int(row[7]);
        s.time_used_ms    = parse_int(row[8]);
        s.memory_used_kb  = parse_int(row[9]);
        s.compile_output  = (row[10] && lens[10]) ? std::string(row[10], lens[10]) : std::string{};
        s.judge_message   = (row[11] && lens[11]) ? std::string(row[11], lens[11]) : std::string{};
        s.created_at      = datetime_to_iso8601(row[12]);
        s.finished_at     = datetime_to_iso8601(row[13]);
        out = std::move(s);
    }
    mysql_free_result(res);
    return out;
}

// ---------------------------------------------------------------------------
//  get_full —— submission + cases
// ---------------------------------------------------------------------------
std::optional<oj::domain::SubmissionDetail>
MysqlSubmissionRepo::get_full(std::int64_t id) {
    auto sub = find_by_id(id);
    if (!sub.has_value()) return std::nullopt;

    oj::domain::SubmissionDetail d;
    d.submission = std::move(*sub);

    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        "SELECT id, case_index, status, time_used_ms, memory_used_kb, "
        "score, is_sample, user_output FROM submission_cases "
        "WHERE submission_id=" + std::to_string(id) + " ORDER BY case_index";
    exec_simple(m, sql, "get_full: cases SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) != 0) throw_stmt(m, "get_full: cases store_result");
        return d;
    }
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::SubmissionCase c;
        c.id              = parse_int64(row[0]);
        c.submission_id   = id;
        c.case_index      = parse_int(row[1]);
        auto r_opt = oj::domain::submission_result_from_string(
            row[2] ? std::string_view(row[2], lens[2]) : std::string_view{});
        c.status          = r_opt.value_or(oj::domain::SubmissionResult::AC);
        c.time_used_ms    = parse_int(row[3]);
        c.memory_used_kb  = parse_int(row[4]);
        c.score           = parse_int(row[5]);
        c.is_sample       = parse_bool(row[6]);
        // 仅 is_sample=1 时 user_output 有内容
        if (c.is_sample && row[7] && lens[7] > 0) {
            c.user_output = std::string(row[7], lens[7]);
        }
        d.cases.push_back(std::move(c));
    }
    mysql_free_result(res);
    return d;
}

// ---------------------------------------------------------------------------
//  list_by_user / list_public_accepted
// ---------------------------------------------------------------------------
namespace {

oj::domain::Submission fetch_submission_row(MYSQL_RES* res) {
    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) return {};
    unsigned long* lens = mysql_fetch_lengths(res);
    oj::domain::Submission s;
    s.id              = parse_int64(row[0]);
    s.user_id         = parse_int64(row[1]);
    s.problem_id      = parse_int64(row[2]);
    auto lang_opt = oj::domain::language_from_string(
        row[3] ? std::string_view(row[3], lens[3]) : std::string_view{});
    s.language        = lang_opt.value_or(oj::domain::Language::Cpp);
    // code 不在列表里拉（可能很大）；按需另查
    s.code.clear();
    auto stat_opt = oj::domain::submission_status_from_string(
        row[4] ? std::string_view(row[4], lens[4]) : std::string_view{});
    s.status          = stat_opt.value_or(oj::domain::SubmissionStatus::Queued);
    if (row[5] && lens[5] > 0) {
        auto r_opt = oj::domain::submission_result_from_string(
            std::string_view(row[5], lens[5]));
        if (r_opt.has_value()) s.result = *r_opt;
    }
    s.total_score     = parse_int(row[6]);
    s.time_used_ms    = parse_int(row[7]);
    s.memory_used_kb  = parse_int(row[8]);
    // compile_output / judge_message 不在列表里
    s.created_at      = datetime_to_iso8601(row[9]);
    s.finished_at     = datetime_to_iso8601(row[10]);
    return s;
}

const char* kListCols =
    "id, user_id, problem_id, language, status, result, "
    "total_score, time_used_ms, memory_used_kb, created_at, finished_at";

}  // namespace

oj::domain::SubmissionListResult
MysqlSubmissionRepo::list_by_user(const oj::domain::SubmissionListQuery& q) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    oj::domain::SubmissionListResult out;
    out.page      = q.page;
    out.page_size = q.page_size;
    if (q.page < 1 || q.page_size < 1) {
        throw std::runtime_error("MysqlSubmissionRepo::list_by_user: invalid page/page_size");
    }

    std::string where = " WHERE 1=1";
    if (q.user_id > 0) {
        where += " AND user_id=" + std::to_string(q.user_id);
    }
    if (q.problem_id > 0) {
        where += " AND problem_id=" + std::to_string(q.problem_id);
    }
    if (q.language.has_value()) {
        where += " AND language=" + quote(*mysql_, oj::domain::to_string(*q.language));
    }
    if (q.status.has_value()) {
        where += " AND status=" + quote(*mysql_, oj::domain::to_string(*q.status));
    }

    // COUNT
    {
        const std::string sql = "SELECT COUNT(*) FROM submissions" + where;
        exec_simple(m, sql, "list_by_user: COUNT");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "list_by_user: store COUNT");
        if (MYSQL_ROW row = mysql_fetch_row(res)) {
            out.total = row[0] ? std::stoll(row[0]) : 0;
        }
        mysql_free_result(res);
    }
    if (out.total == 0) return out;

    const int offset = (q.page - 1) * q.page_size;
    const std::string sql =
        std::string{"SELECT "} + kListCols + " FROM submissions" + where +
        " ORDER BY created_at DESC, id DESC LIMIT " + std::to_string(q.page_size) +
        " OFFSET " + std::to_string(offset);
    exec_simple(m, sql, "list_by_user: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "list_by_user: store items");
    while (true) {
        oj::domain::Submission s = fetch_submission_row(res);
        if (s.id == 0) break;
        out.items.push_back(std::move(s));
    }
    mysql_free_result(res);
    return out;
}

oj::domain::SubmissionListResult
MysqlSubmissionRepo::list_public_accepted(const oj::domain::SubmissionListQuery& q) {
    // 公共列表 = 仅 result='AC' + status='finished'
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    oj::domain::SubmissionListResult out;
    out.page      = q.page;
    out.page_size = q.page_size;
    if (q.page < 1 || q.page_size < 1) {
        throw std::runtime_error("MysqlSubmissionRepo::list_public_accepted: invalid page/page_size");
    }

    std::string where = " WHERE result='AC' AND status='finished'";
    if (q.problem_id > 0) {
        where += " AND problem_id=" + std::to_string(q.problem_id);
    }
    if (q.language.has_value()) {
        where += " AND language=" + quote(*mysql_, oj::domain::to_string(*q.language));
    }

    // COUNT
    {
        const std::string sql = "SELECT COUNT(*) FROM submissions" + where;
        exec_simple(m, sql, "list_public_accepted: COUNT");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "list_public_accepted: store COUNT");
        if (MYSQL_ROW row = mysql_fetch_row(res)) {
            out.total = row[0] ? std::stoll(row[0]) : 0;
        }
        mysql_free_result(res);
    }
    if (out.total == 0) return out;

    const int offset = (q.page - 1) * q.page_size;
    const std::string sql =
        std::string{"SELECT "} + kListCols + " FROM submissions" + where +
        " ORDER BY created_at DESC, id DESC LIMIT " + std::to_string(q.page_size) +
        " OFFSET " + std::to_string(offset);
    exec_simple(m, sql, "list_public_accepted: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "list_public_accepted: store items");
    while (true) {
        oj::domain::Submission s = fetch_submission_row(res);
        if (s.id == 0) break;
        out.items.push_back(std::move(s));
    }
    mysql_free_result(res);
    return out;
}

}  // namespace oj::infra