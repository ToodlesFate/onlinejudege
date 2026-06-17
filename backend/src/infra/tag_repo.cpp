// =============================================================================
//  MysqlTagRepo 实现
// =============================================================================

#include "infra/tag_repo.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
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
    throw std::runtime_error(std::string{"MysqlTagRepo: "} + what + ": [" +
                             sqlstate + "] " + err);
}

void exec_simple(MYSQL* m, const std::string& sql, const char* what) {
    if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
        throw_stmt(m, what);
    }
}

int parse_int(const char* raw) {
    return raw ? std::stoi(raw) : 0;
}

std::optional<oj::domain::Tag>
fetch_tag(MYSQL* m, const std::string& sql) {
    exec_simple(m, sql, "fetch_tag: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) {
        if (mysql_field_count(m) == 0) return std::nullopt;
        throw_stmt(m, "fetch_tag: store_result");
    }
    std::optional<oj::domain::Tag> out;
    if (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Tag t;
        t.id   = parse_int(row[0]);
        t.name = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        t.slug = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        out = std::move(t);
    }
    mysql_free_result(res);
    return out;
}

}  // namespace

std::vector<oj::domain::Tag> MysqlTagRepo::list_all() {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql = "SELECT id, name, slug FROM tags ORDER BY id";
    exec_simple(m, sql, "list_all: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "list_all: store_result");
    std::vector<oj::domain::Tag> out;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Tag t;
        t.id   = parse_int(row[0]);
        t.name = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        t.slug = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        out.push_back(std::move(t));
    }
    mysql_free_result(res);
    return out;
}

std::optional<oj::domain::Tag> MysqlTagRepo::find_by_id(int id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    return fetch_tag(m, "SELECT id, name, slug FROM tags WHERE id=" +
                          std::to_string(id) + " LIMIT 1");
}

std::optional<oj::domain::Tag>
MysqlTagRepo::find_by_slug(const std::string& slug) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    return fetch_tag(m, "SELECT id, name, slug FROM tags WHERE slug=" +
                          quote(*mysql_, slug) + " LIMIT 1");
}

std::vector<oj::domain::Tag>
MysqlTagRepo::find_by_ids(const std::vector<int>& ids) {
    if (ids.empty()) return {};
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    std::string in_list;
    in_list.reserve(ids.size() * 6);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i) in_list.push_back(',');
        in_list += std::to_string(ids[i]);
    }
    // 一次 IN 查询；保持"按入参顺序"返回（list 接口用得到）
    // —— 用 map 缓存 id → tag，最后按 ids 顺序收集
    const std::string sql = "SELECT id, name, slug FROM tags WHERE id IN (" +
                            in_list + ")";
    exec_simple(m, sql, "find_by_ids: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "find_by_ids: store_result");
    std::unordered_map<int, oj::domain::Tag> by_id;
    by_id.reserve(ids.size());
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Tag t;
        t.id   = parse_int(row[0]);
        t.name = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        t.slug = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        by_id.emplace(t.id, std::move(t));
    }
    mysql_free_result(res);
    std::vector<oj::domain::Tag> out;
    out.reserve(ids.size());
    for (int id : ids) {
        auto it = by_id.find(id);
        if (it != by_id.end()) out.push_back(std::move(it->second));
    }
    return out;
}

std::vector<oj::domain::Tag>
MysqlTagRepo::tags_of_problem(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql =
        "SELECT t.id, t.name, t.slug "
        "FROM tags t JOIN problem_tags pt ON pt.tag_id = t.id "
        "WHERE pt.problem_id=" + std::to_string(problem_id) + " ORDER BY t.id";
    exec_simple(m, sql, "tags_of_problem: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "tags_of_problem: store");
    std::vector<oj::domain::Tag> out;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lens = mysql_fetch_lengths(res);
        oj::domain::Tag t;
        t.id   = parse_int(row[0]);
        t.name = (row[1] && lens[1]) ? std::string(row[1], lens[1]) : std::string{};
        t.slug = (row[2] && lens[2]) ? std::string(row[2], lens[2]) : std::string{};
        out.push_back(std::move(t));
    }
    mysql_free_result(res);
    return out;
}

std::vector<int>
MysqlTagRepo::tag_ids_of_problem(std::int64_t problem_id) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();
    const std::string sql = "SELECT tag_id FROM problem_tags WHERE problem_id=" +
                            std::to_string(problem_id) + " ORDER BY tag_id";
    exec_simple(m, sql, "tag_ids_of_problem: SELECT");
    MYSQL_RES* res = mysql_store_result(m);
    if (res == nullptr) throw_stmt(m, "tag_ids_of_problem: store");
    std::vector<int> out;
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        out.push_back(parse_int(row[0]));
    }
    mysql_free_result(res);
    return out;
}

void MysqlTagRepo::set_problem_tags(std::int64_t problem_id,
                                    const std::vector<int>& tag_ids) {
    auto lease = mysql_->acquire();
    MYSQL* m = lease.raw();

    const std::string kStartTxn = "START TRANSACTION";
    const std::string kCommit   = "COMMIT";
    const std::string kRollback = "ROLLBACK";
    exec_simple(m, kStartTxn, "set_problem_tags: START TRANSACTION");
    bool committed = false;
    struct RollbackGuard {
        MYSQL* m;
        const std::string* rollback_sql;
        bool* flag;
        ~RollbackGuard() { if (!*flag) { mysql_real_query(m, rollback_sql->data(), static_cast<unsigned long>(rollback_sql->size())); } }
    } guard{m, &kRollback, &committed};

    // 1) DELETE 旧关联
    const std::string del_sql = "DELETE FROM problem_tags WHERE problem_id=" +
                                 std::to_string(problem_id);
    exec_simple(m, del_sql, "set_problem_tags: DELETE");

    // 2) INSERT 新关联
    for (int tid : tag_ids) {
        const std::string sql = "INSERT INTO problem_tags (problem_id, tag_id) VALUES (" +
                                std::to_string(problem_id) + "," +
                                std::to_string(tid) + ")";
        if (mysql_real_query(m, sql.data(), sql.size()) != 0) {
            throw_stmt(m, "set_problem_tags: INSERT");
        }
    }
    exec_simple(m, kCommit, "set_problem_tags: COMMIT");
    committed = true;
    spdlog::info("MysqlTagRepo::set_problem_tags problem_id={} n={}", problem_id, tag_ids.size());
}

}  // namespace oj::infra
