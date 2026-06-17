// =============================================================================
//  MysqlProblemRepo 实现 —— 风格与 MysqlUserRepo 完全一致：
//    1) escape + quote 拼 SQL
//    2) START TRANSACTION / COMMIT / ROLLBACK with RollbackGuard
//    3) escape 通过 MysqlClient::escape (mysql_real_escape_string)
// =============================================================================

#include "infra/problem_repo.hpp"

#include <chrono>
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

// 错误码
constexpr unsigned int kErDupEntry  = 1062;   // ER_DUP_ENTRY
constexpr unsigned int kErDeadlock  = 1213;
constexpr unsigned int kErLockWait  = 1205;
constexpr int          kDeadlockMaxRetries = 3;

std::string quote(MysqlClient& cli, std::string_view s) {
    return "'" + cli.escape(s) + "'";
}

[[noreturn]] void throw_stmt(MYSQL* m, const char* what) {
    const char* err      = mysql_error(m);
    const char* sqlstate = mysql_sqlstate(m);
    throw std::runtime_error(std::string{"MysqlProblemRepo: "} + what + ": [" +
                             sqlstate + "] " + err);
}

void exec_simple(MYSQL* m, const std::string& sql, const char* what) {
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, what);
    }
}

// 读取 DATETIME 列：mysql 返回 "YYYY-MM-DD HH:MM:SS"
// 转成 SPEC §5.3 的 "YYYY-MM-DDTHH:MM:SSZ"
std::string datetime_to_iso8601(const char* raw) {
    if (!raw) return {};
    std::string s(raw);
    // 把第一个 ' ' 替换为 'T'，末尾追加 'Z' (UTC 标记)
    auto sp = s.find(' ');
    if (sp != std::string::npos) s[sp] = 'T';
    s.push_back('Z');
    return s;
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

// 把 Problem 行 SELECT 出来（不含 testcases / tags）
// 取的列顺序：id, title, content_md, difficulty, time_limit_ms, memory_limit_mb,
//              output_limit_mb, is_published, created_by, created_at
std::optional<oj::domain::Problem>
fetch_problem_row(MYSQL* m, const std::string& sql) {
    exec_simple(m, sql, "fetch_problem_row: query");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) == 0) return std::nullopt;
        throw_stmt(m, "fetch_problem_row: store_result");
    }
    std::optional<oj::domain::Problem> out;
    if (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Problem p;
        p.id            = parse_int64(row[0]);
        p.title         = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        p.content_md    = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        auto d_opt = oj::domain::difficulty_from_string(
            row[3] ? std::string_view(row[3], lens[3]) : std::string_view{});
        p.difficulty    = d_opt.value_or(oj::domain::Difficulty::Easy);
        p.time_limit_ms  = parse_int(row[4]);
        p.memory_limit_mb = parse_int(row[5]);
        p.output_limit_mb = parse_int(row[6]);
        p.is_published   = parse_bool(row[7]);
        p.created_by     = parse_int64(row[8]);
        p.created_at     = datetime_to_iso8601(row[9]);
        out = std::move(p);
    }
    mysql_free_result(res);
    return out;
}

const char* kProblemCols =
    "id, title, content_md, difficulty, time_limit_ms, memory_limit_mb, "
    "output_limit_mb, is_published, created_by, created_at";

}  // namespace

// ---------------------------------------------------------------------------
//  find_by_id
// ---------------------------------------------------------------------------
std::optional<oj::domain::Problem>
MysqlProblemRepo::find_by_id(std::int64_t id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        std::string{"SELECT "} + kProblemCols + " FROM problems WHERE id=" +
        std::to_string(id) + " LIMIT 1";
    return fetch_problem_row(m, sql);
}

// ---------------------------------------------------------------------------
//  list —— 分页 + 过滤 + 排序
//
//  SQL 构造策略：
//    WHERE 1=1
//      AND (include_unpublished OR is_published=1)
//      AND (? OR difficulty = ?)
//      AND (? OR NOT EXISTS (1 FROM problem_tags WHERE problem_id=p.id AND tag_id NOT IN (...)))
//      AND (? OR title LIKE '%...%')
//    ORDER BY ...
//    LIMIT ? OFFSET ?
//
//  关联 tag 用 NOT EXISTS + IN 表达 AND 语义：
//    "题目必须拥有所有指定 tag"
//    转化为：不存在 "该题目拥有但不属于指定集合" 的 tag 关联
//    等价于：题目拥有的 tag 集合 ⊆ 指定的 tag 集合 —— 即"全部命中"
//    若 tag_slugs 为空 → 退化为 "不筛"
// ---------------------------------------------------------------------------
oj::domain::ProblemListResult
MysqlProblemRepo::list(const oj::domain::ProblemListQuery& q) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    oj::domain::ProblemListResult out;
    out.page      = q.page;
    out.page_size = q.page_size;
    if (q.page < 1 || q.page_size < 1) {
        throw std::runtime_error("MysqlProblemRepo::list: invalid page/page_size");
    }

    // ----- 1) 解析 tag_slug → tag_id -----
    std::vector<int> tag_ids;
    if (!q.tag_slugs.empty()) {
        std::string in_list;
        in_list.reserve(q.tag_slugs.size() * 8);
        for (std::size_t i = 0; i < q.tag_slugs.size(); ++i) {
            if (i) in_list.push_back(',');
            in_list += quote(*mysql_, q.tag_slugs[i]);
        }
        const std::string sql = "SELECT id FROM tags WHERE slug IN (" + in_list + ")";
        exec_simple(m, sql, "list: tag slug lookup");
        MYSQL_RES* res = mysql_store_result(m);
        if (res == nullptr) throw_stmt(m, "list: store tag id");
        while (MYSQL_ROW row = mysql_fetch_row(res)) {
            if (row[0]) tag_ids.push_back(parse_int(row[0]));
        }
        mysql_free_result(res);
    }

    // ----- 2) 构造 WHERE / ORDER BY / LIMIT -----
    std::string where = " WHERE 1=1";
    if (!q.include_unpublished) {
        where += " AND p.is_published=1";
    }
    if (q.difficulty.has_value()) {
        where += " AND p.difficulty=" + quote(*mysql_, oj::domain::to_string(*q.difficulty));
    }
    if (!q.q.empty()) {
        // title LIKE '%q%'；按 byte 转义已用 quote() 包裹
        where += " AND p.title LIKE " + quote(*mysql_, "%" + q.q + "%");
    }
    if (!tag_ids.empty()) {
        // AND 语义：题目必须**同时拥有**全部指定 tag
        // 实现：对每条 problem 统计"在指定集合里的 tag 数"，
        //       与指定 tag 总数一致才命中
        // SELECT COUNT(DISTINCT tag_id) FROM problem_tags
        //  WHERE problem_id = p.id AND tag_id IN (...) = N
        // 比 NOT EXISTS 更直观："specified ⊆ problem.tags" 而非 "problem.tags ⊆ specified"
        std::string in_list;
        in_list.reserve(tag_ids.size() * 6);
        for (std::size_t i = 0; i < tag_ids.size(); ++i) {
            if (i) in_list.push_back(',');
            in_list += std::to_string(tag_ids[i]);
        }
        where += " AND ("
                 "SELECT COUNT(DISTINCT tag_id) FROM problem_tags "
                 "WHERE problem_id=p.id AND tag_id IN (" + in_list + ")"
                 ") = " + std::to_string(tag_ids.size());
    }

    std::string order_by;
    switch (q.sort) {
        case oj::domain::ProblemListQuery::Sort::IdDesc:
            order_by = " ORDER BY p.id DESC";
            break;
        case oj::domain::ProblemListQuery::Sort::CreatedDesc:
            order_by = " ORDER BY p.created_at DESC, p.id DESC";
            break;
        case oj::domain::ProblemListQuery::Sort::PassRateDesc:
            // 通过率在子查询里实时算；NULL → -1 让无数据的题沉底
            order_by =
                " ORDER BY (CASE WHEN ps.total IS NULL OR ps.total=0 THEN -1.0 "
                "             ELSE ps.accepted*1.0/ps.total END) DESC, p.id DESC";
            break;
    }

    const int offset = (q.page - 1) * q.page_size;
    // 用 LEFT JOIN 一个子查询拿每题的 (total, accepted)。
    // 该 join 对每种 sort 都必要 —— SELECT 列表里 ps.total / ps.accepted
    // 在 ProblemListItem 里是必填字段；与 ORDER BY 是否用它无关。
    // 题目量不大（< 数千）+ submissions 走 status='finished' 过滤时走 idx_submissions_result
    // 索引，开销可接受。
    const std::string join_stats =
        " LEFT JOIN ("
        "  SELECT problem_id, COUNT(*) AS total, "
        "         SUM(CASE WHEN result='AC' THEN 1 ELSE 0 END) AS accepted "
        "  FROM submissions WHERE status='finished' "
        "  GROUP BY problem_id"
        ") ps ON ps.problem_id = p.id";

    // ----- 3) 统计总数（用同样 WHERE，单独跑一遍） -----
    const std::string count_sql = "SELECT COUNT(*) FROM problems p" + join_stats + where;
    exec_simple(m, count_sql, "list: COUNT");
    MYSQL_RES* cres = mysql_store_result(m);
    if (cres == nullptr) throw_stmt(m, "list: store COUNT");
    if (MYSQL_ROW r = mysql_fetch_row(cres)) {
        out.total = r[0] ? std::stoll(r[0]) : 0;
    }
    mysql_free_result(cres);

    if (out.total == 0) return out;  // 提前返回，避免空跑 list SQL

    // ----- 4) 拉本页 items -----
    std::string sql = "SELECT p.id, p.title, p.difficulty, p.is_published, "
                      "       p.created_by, p.created_at, "
                      "       COALESCE(ps.total, 0), COALESCE(ps.accepted, 0) "
                      "FROM problems p" + join_stats + where + order_by +
                      " LIMIT " + std::to_string(q.page_size) +
                      " OFFSET " + std::to_string(offset);
    exec_simple(m, sql, "list: SELECT items");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "list: store items");
    std::vector<std::int64_t> item_ids;
    item_ids.reserve(q.page_size);
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        oj::domain::ProblemListItem it;
        it.id                   = parse_int64(row[0]);
        unsigned long* lens     = mysql_fetch_lengths(res);
        it.title                = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        auto d_opt = oj::domain::difficulty_from_string(
            row[2] ? std::string_view(row[2], lens[2]) : std::string_view{});
        it.difficulty           = d_opt.value_or(oj::domain::Difficulty::Easy);
        it.is_published         = parse_bool(row[3]);
        it.created_by           = parse_int64(row[4]);
        it.created_at           = datetime_to_iso8601(row[5]);
        it.total_submissions    = parse_int(row[6]);
        it.accepted_submissions = parse_int(row[7]);
        out.items.push_back(std::move(it));
        item_ids.push_back(out.items.back().id);
    }
    mysql_free_result(res);

    // ----- 5) 一次拉所有 item 的 tag，组装 -----
    if (!item_ids.empty()) {
        std::string in_list;
        in_list.reserve(item_ids.size() * 8);
        for (std::size_t i = 0; i < item_ids.size(); ++i) {
            if (i) in_list.push_back(',');
            in_list += std::to_string(item_ids[i]);
        }
        const std::string t_sql =
            "SELECT pt.problem_id, t.id, t.name, t.slug "
            "FROM problem_tags pt JOIN tags t ON t.id = pt.tag_id "
            "WHERE pt.problem_id IN (" + in_list + ") "
            "ORDER BY pt.problem_id, t.id";
        exec_simple(m, t_sql, "list: tag lookup");
        MYSQL_RES* tres = mysql_store_result(m);
        if (tres == nullptr) throw_stmt(m, "list: store tag lookup");
        // 建索引：problem_id → item 位置
        std::vector<std::int64_t> ids_copy = item_ids;  // already sorted by id DESC
        auto find_idx = [&ids_copy](std::int64_t pid) -> std::size_t {
            for (std::size_t i = 0; i < ids_copy.size(); ++i) {
                if (ids_copy[i] == pid) return i;
            }
            return static_cast<std::size_t>(-1);
        };
        while (MYSQL_ROW row = mysql_fetch_row(tres)) {
            std::int64_t pid = parse_int64(row[0]);
            auto idx = find_idx(pid);
            if (idx == static_cast<std::size_t>(-1)) continue;
            oj::domain::Tag t;
            // mysql_fetch_lengths 必须配最近一次 mysql_fetch_row 所在的结果集；
            // items 的 res 已经在前面 free 了，这里只能用 tres
            unsigned long* lens = mysql_fetch_lengths(tres);
            t.id   = parse_int(row[1]);
            t.name = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
            t.slug = (row[3] && lens[3]) ? std::string(row[3], lens[3]) : std::string{};
            if (idx < out.items.size()) out.items[idx].tags.push_back(std::move(t));
        }
        mysql_free_result(tres);
    }

    return out;
}

// ---------------------------------------------------------------------------
//  create
// ---------------------------------------------------------------------------
oj::domain::Problem MysqlProblemRepo::create(const oj::domain::Problem& p) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string kStartTxn = "START TRANSACTION";
    const std::string kCommit   = "COMMIT";
    const std::string kRollback = "ROLLBACK";

    exec_simple(m, kStartTxn, "create: START TRANSACTION");
    bool committed = false;
    struct RollbackGuard {
        MYSQL* m;
        const std::string* rollback_sql;
        bool*  flag;
        ~RollbackGuard() { if (!*flag) { mysql_real_query(m, rollback_sql->data(), static_cast<unsigned long>(rollback_sql->size())); } }
    } guard{m, &kRollback, &committed};

    const std::string sql =
        "INSERT INTO problems "
        "(title, content_md, difficulty, time_limit_ms, memory_limit_mb, "
        " output_limit_mb, is_published, created_by) VALUES (" +
        quote(*mysql_, p.title) + "," +
        quote(*mysql_, p.content_md) + "," +
        quote(*mysql_, oj::domain::to_string(p.difficulty)) + "," +
        std::to_string(p.time_limit_ms) + "," +
        std::to_string(p.memory_limit_mb) + "," +
        std::to_string(p.output_limit_mb) + "," +
        (p.is_published ? "1" : "0") + "," +
        std::to_string(p.created_by) + ")";

    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, "create: INSERT");
    }
    const std::int64_t new_id = static_cast<std::int64_t>(mysql_insert_id(m));

    exec_simple(m, kCommit, "create: COMMIT");
    committed = true;

    spdlog::info("MysqlProblemRepo::create id={} title='{}' created_by={}",
                 new_id, p.title, p.created_by);

    // 回查拿到 created_at；用 find_by_id 即可
    auto fetched = find_by_id(new_id);
    if (!fetched.has_value()) {
        throw std::runtime_error("MysqlProblemRepo::create: post-fetch failed");
    }
    return *fetched;
}

// ---------------------------------------------------------------------------
//  update —— 全量更新
// ---------------------------------------------------------------------------
void MysqlProblemRepo::update(const oj::domain::Problem& p) {
    if (p.id <= 0) {
        throw std::runtime_error("MysqlProblemRepo::update: id required");
    }
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        "UPDATE problems SET "
        "title="          + quote(*mysql_, p.title) + ","
        "content_md="     + quote(*mysql_, p.content_md) + ","
        "difficulty="     + quote(*mysql_, oj::domain::to_string(p.difficulty)) + ","
        "time_limit_ms="  + std::to_string(p.time_limit_ms) + ","
        "memory_limit_mb=" + std::to_string(p.memory_limit_mb) + ","
        "output_limit_mb=" + std::to_string(p.output_limit_mb) + ","
        "is_published="   + (p.is_published ? "1" : "0") + " "
        "WHERE id="       + std::to_string(p.id);

    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, "update: UPDATE");
    }
    if (mysql_affected_rows(m) == 0) {
        throw std::runtime_error("MysqlProblemRepo::update: id not found: " +
                                 std::to_string(p.id));
    }
    spdlog::info("MysqlProblemRepo::update id={}", p.id);
}

// ---------------------------------------------------------------------------
//  soft_delete —— is_published=0
// ---------------------------------------------------------------------------
void MysqlProblemRepo::soft_delete(std::int64_t id) {
    set_published(id, false);
}

// ---------------------------------------------------------------------------
//  set_published
// ---------------------------------------------------------------------------
void MysqlProblemRepo::set_published(std::int64_t id, bool published) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql = "UPDATE problems SET is_published=" +
                            std::string{published ? "1" : "0"} +
                            " WHERE id=" + std::to_string(id);
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, "set_published: UPDATE");
    }
    if (mysql_affected_rows(m) == 0) {
        throw std::runtime_error("MysqlProblemRepo::set_published: id not found: " +
                                 std::to_string(id));
    }
    spdlog::info("MysqlProblemRepo::set_published id={} published={}", id, published);
}

// ---------------------------------------------------------------------------
//  submission_stats
// ---------------------------------------------------------------------------
std::pair<int, int>
MysqlProblemRepo::submission_stats(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string sql =
        "SELECT COUNT(*) AS total, "
        "       COALESCE(SUM(CASE WHEN result='AC' THEN 1 ELSE 0 END), 0) AS accepted "
        "FROM submissions "
        "WHERE problem_id=" + std::to_string(problem_id) +
        "  AND status='finished'";
    exec_simple(m, sql, "submission_stats: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "submission_stats: store");
    std::pair<int, int> out{0, 0};
    if (MYSQL_ROW row = mysql_fetch_row(res)) {
        out.first  = parse_int(row[0]);
        out.second = parse_int(row[1]);
    }
    mysql_free_result(res);
    return out;
}

}  // namespace oj::infra
