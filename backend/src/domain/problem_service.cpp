// =============================================================================
//  problem_service.cpp —— ProblemService + URL 解析
// =============================================================================

#include "domain/problem_service.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include <spdlog/spdlog.h>

namespace oj::domain {

namespace {

// trim 前后空白
std::string trim(std::string_view s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

// split by ','，每个 token 单独 trim，去空，dedup（保序）
std::vector<std::string> split_csv_dedup(const std::string& s) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::size_t start = 0;
    while (start <= s.size()) {
        std::size_t end = s.find(',', start);
        if (end == std::string::npos) end = s.size();
        std::string tok = trim(std::string_view(s).substr(start, end - start));
        if (!tok.empty() && seen.insert(tok).second) {
            out.push_back(std::move(tok));
        }
        if (end == s.size()) break;
        start = end + 1;
    }
    return out;
}

// page_size 白名单校验；非法 → 默认值
int sanitize_page_size(int s) {
    for (int opt : kProblemPageSizeOptions) {
        if (s == opt) return s;
    }
    return kProblemDefaultPageSize;
}

}  // namespace

// ---------------------------------------------------------------------------
//  parse_problems_list_query —— URL 原始参数 → ProblemListQuery
//
//  关键规则（与 SPEC §2.2.2 / §3.3.5 G / §5.2.2 一一对应）：
//   - page < 1 → 1；page_size ∉ {10,20,50} → 20
//   - difficulty / sort 字符串解析失败 → 保持默认值
//   - tag 接受多次出现 + 逗号分隔
//   - 公共 API 永远只返回已发布（include_unpublished 强制为 false）；
//     admin 路径调用时 is_admin=true 才允许 true
//
//  错误返回：非法整数 / 越界等"客户端输入有问题"的统一通过 error_message 反馈；
//  这层不抛异常，由 handler 决定映射 1001 BadRequest。
// ---------------------------------------------------------------------------
ParsedListQuery parse_problems_list_query(
    const std::multimap<std::string, std::string>& params,
    bool is_admin)
{
    ParsedListQuery out;
    ProblemListQuery& q = out.query;

    // page
    {
        auto it = params.find("page");
        if (it != params.end()) {
            try {
                int p = std::stoi(it->second);
                if (p < 1) {
                    out.error_message = "page must be >= 1";
                } else {
                    q.page = p;
                }
            } catch (...) {
                out.error_message = "page must be an integer";
            }
        }
    }

    // size
    {
        auto it = params.find("size");
        if (it != params.end()) {
            try {
                int s = std::stoi(it->second);
                q.page_size = sanitize_page_size(s);
                // 提示但不报错 —— 不在白名单时按 SPEC §2.2.2 走默认值
                if (s != q.page_size) {
                    // 不设 error_message，只静默 fallback
                }
            } catch (...) {
                q.page_size = kProblemDefaultPageSize;
            }
        }
    }

    // difficulty —— 解析失败静默忽略
    if (auto it = params.find("difficulty"); it != params.end()) {
        auto d = difficulty_from_string(it->second);
        if (d) q.difficulty = *d;
    }

    // sort —— 解析失败用默认 IdDesc
    if (auto it = params.find("sort"); it != params.end()) {
        const std::string& s = it->second;
        if      (s == "id_desc")        q.sort = ProblemListQuery::Sort::IdDesc;
        else if (s == "created_desc")   q.sort = ProblemListQuery::Sort::CreatedDesc;
        else if (s == "pass_rate_desc") q.sort = ProblemListQuery::Sort::PassRateDesc;
        // 其他值：保持默认
    }

    // tag —— 多次出现 + 逗号分隔 + dedup
    {
        std::set<std::string> seen;
        auto range = params.equal_range("tag");
        for (auto it = range.first; it != range.second; ++it) {
            for (auto& tok : split_csv_dedup(it->second)) {
                if (seen.insert(tok).second) q.tag_slugs.push_back(std::move(tok));
            }
        }
    }

    // q
    if (auto it = params.find("q"); it != params.end()) {
        q.q = it->second;
    }

    // include_unpublished —— 公共 API 强制 false，admin 才尊重入参
    if (is_admin) {
        auto it = params.find("include_unpublished");
        if (it != params.end()) {
            const std::string& v = it->second;
            q.include_unpublished = !(v == "0" || v == "false" || v == "False");
        }
    } else {
        q.include_unpublished = false;
    }

    return out;
}

// ---------------------------------------------------------------------------
//  ProblemService::list —— 业务校验 + 委托 repo
// ---------------------------------------------------------------------------
ProblemListResult ProblemService::list(const ProblemListQuery& q) {
    ProblemListQuery sane = q;
    if (sane.page < 1) sane.page = 1;
    sane.page_size = sanitize_page_size(sane.page_size);
    return problems_->list(sane);
}

// ---------------------------------------------------------------------------
//  ProblemService::get_detail
//
//  流程：
//   1) repo.find_by_id(id) → 找不到 / 公开访问但 is_published=0 → nullopt
//   2) tags_of_problem(id) → 关联 tag 列表
//   3) list_samples(id) → 仅 is_sample=1 的样例
// ---------------------------------------------------------------------------
std::optional<ProblemDetail>
ProblemService::get_detail(std::int64_t id, bool include_unpublished) {
    auto p = problems_->find_by_id(id);
    if (!p.has_value()) return std::nullopt;
    if (!include_unpublished && !p->is_published) return std::nullopt;

    ProblemDetail d;
    d.problem          = std::move(*p);
    d.tags             = tags_      ? tags_->tags_of_problem(id)         : std::vector<Tag>{};
    d.sample_testcases = testcases_ ? testcases_->list_samples(id)       : std::vector<Testcase>{};
    return d;
}

std::vector<Tag> ProblemService::list_tags() {
    if (!tags_) return {};
    return tags_->list_all();
}

// ===========================================================================
//  后台管理实现（SPEC §5.2.4 admin API）
// ===========================================================================
//
// 错误约定：所有业务校验失败 → 抛 AdminProblemError(BadRequest, msg)；
// repo 抛 std::runtime_error → 翻译为 AdminProblemError(Internal, ...)。
// 这样 handler 只需 switch kind 即可映射到 ErrorCode。

namespace {

std::string trim_copy(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

[[noreturn]] void throw_bad(const std::string& msg) {
    throw oj::domain::AdminProblemError(
        oj::domain::AdminProblemErrorKind::BadRequest, msg);
}

// repo 抛 std::runtime_error → 翻译为 Internal；同时把原信息保留到 spdlog
[[noreturn]] void rethrow_as_internal(const std::string& where,
                                      const std::exception& e) {
    spdlog::error("ProblemService::{} internal error: {}", where, e.what());
    throw oj::domain::AdminProblemError(
        oj::domain::AdminProblemErrorKind::Internal,
        std::string{"internal error in "} + where);
}

#define OJ_THROW_BAD(msg) throw_bad(msg)
#define OJ_CATCH_RETHROW(where) \
    catch (const oj::domain::AdminProblemError&) { throw; } \
    catch (const std::exception& e) { rethrow_as_internal(where, e); }

}  // namespace

// ---------------------------------------------------------------------------
//  get_admin_detail —— 含全部测试点；admin 编辑表单用
// ---------------------------------------------------------------------------
std::optional<AdminProblemDetail>
ProblemService::get_admin_detail(std::int64_t id) {
    try {
        auto p = problems_->find_by_id(id);
        if (!p.has_value()) return std::nullopt;

        AdminProblemDetail d;
        d.problem   = std::move(*p);
        d.tags      = tags_      ? tags_->tags_of_problem(id)    : std::vector<Tag>{};
        d.testcases = testcases_ ? testcases_->list_by_problem(id) : std::vector<Testcase>{};
        return d;
    } OJ_CATCH_RETHROW("get_admin_detail")
}

// ---------------------------------------------------------------------------
//  validate_cases —— 集中校验 1..100 个 / 序号唯一 / score 之和=100
//
//  校验失败抛 AdminProblemError(BadRequest)；通过则继续。
// ---------------------------------------------------------------------------
void ProblemService::validate_cases(const std::vector<Testcase>& cases) {
    if (static_cast<int>(cases.size()) < kProblemCaseMin) {
        OJ_THROW_BAD("at least 1 testcase is required");
    }
    if (static_cast<int>(cases.size()) > kProblemCaseMax) {
        OJ_THROW_BAD("at most 100 testcases are allowed");
    }

    std::unordered_set<int> seen_idx;
    long long score_sum = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const auto& c = cases[i];
        if (c.case_index < 1 || c.case_index > kProblemCaseMax) {
            OJ_THROW_BAD("case[" + std::to_string(i) + "].case_index must be in [1,100]");
        }
        if (!seen_idx.insert(c.case_index).second) {
            OJ_THROW_BAD("duplicate case_index: " + std::to_string(c.case_index));
        }
        if (c.score < 0 || c.score > kProblemCaseScoreSum) {
            OJ_THROW_BAD("case[" + std::to_string(i) + "].score must be in [0,100]");
        }
        score_sum += c.score;
    }
    if (score_sum != kProblemCaseScoreSum) {
        OJ_THROW_BAD("sum of all testcase scores must be 100, got " +
                     std::to_string(score_sum));
    }
}

// ---------------------------------------------------------------------------
//  validate_tag_ids —— 校验所有 tag id 都真实存在
//
//  失败抛 AdminProblemError(BadRequest)。空数组直接通过。
// ---------------------------------------------------------------------------
void ProblemService::validate_tag_ids(const std::vector<int>& tag_ids) {
    if (tag_ids.empty()) return;
    std::unordered_set<int> seen;
    for (int id : tag_ids) {
        if (id <= 0) {
            OJ_THROW_BAD("tag_ids must be positive integers");
        }
        if (!seen.insert(id).second) {
            OJ_THROW_BAD("duplicate tag_id: " + std::to_string(id));
        }
        if (tags_ && !tags_->find_by_id(id).has_value()) {
            OJ_THROW_BAD("tag_id not found: " + std::to_string(id));
        }
    }
}

// ---------------------------------------------------------------------------
//  build_problem —— 把 ProblemWriteInput 翻译成 Problem（业务校验）
// ---------------------------------------------------------------------------
Problem ProblemService::build_problem(const ProblemWriteInput& in) const {
    // 1) title —— trim 后 1..100 字符
    const std::string title = trim_copy(in.title);
    if (title.empty()) {
        OJ_THROW_BAD("title is required");
    }
    if (title.size() > kProblemTitleMax) {
        OJ_THROW_BAD("title must be at most " +
                     std::to_string(kProblemTitleMax) + " characters");
    }

    // 2) content_md —— 1..64KB
    if (in.content_md.empty()) {
        OJ_THROW_BAD("content_md is required");
    }
    if (in.content_md.size() > kProblemContentMdMax) {
        OJ_THROW_BAD("content_md must be at most " +
                     std::to_string(kProblemContentMdMax) + " bytes");
    }

    // 3) difficulty
    auto d = difficulty_from_string(in.difficulty_str);
    if (!d.has_value()) {
        OJ_THROW_BAD("difficulty must be one of: easy, medium, hard");
    }

    // 4) limits
    if (in.time_limit_ms < kProblemTimeLimitMin ||
        in.time_limit_ms > kProblemTimeLimitMax) {
        OJ_THROW_BAD("time_limit_ms must be in [" +
                     std::to_string(kProblemTimeLimitMin) + ", " +
                     std::to_string(kProblemTimeLimitMax) + "]");
    }
    if (in.memory_limit_mb < kProblemMemoryLimitMin ||
        in.memory_limit_mb > kProblemMemoryLimitMax) {
        OJ_THROW_BAD("memory_limit_mb must be in [" +
                     std::to_string(kProblemMemoryLimitMin) + ", " +
                     std::to_string(kProblemMemoryLimitMax) + "]");
    }
    if (in.output_limit_mb < kProblemOutputLimitMin ||
        in.output_limit_mb > kProblemOutputLimitMax) {
        OJ_THROW_BAD("output_limit_mb must be in [" +
                     std::to_string(kProblemOutputLimitMin) + ", " +
                     std::to_string(kProblemOutputLimitMax) + "]");
    }

    Problem p;
    p.title            = title;
    p.content_md       = in.content_md;
    p.difficulty       = *d;
    p.time_limit_ms    = in.time_limit_ms;
    p.memory_limit_mb  = in.memory_limit_mb;
    p.output_limit_mb  = in.output_limit_mb;
    p.is_published     = in.is_published;
    return p;
}

#undef OJ_THROW_BAD
#undef OJ_CATCH_RETHROW

// ---------------------------------------------------------------------------
//  create_problem
//
//  事务边界：problem 行 → set_problem_tags → create_many testcases
//  repo 各方法本身已用事务 / 顺序执行；任一失败上层抛 AdminProblemError。
//  注意：set_problem_tags / create_many 当前实现**各自**走一个事务；本层
//  不再额外包事务（避免长事务），失败时由 service 主动回滚：删除刚建的
//  problem（testcases/problem_tags 走 ON DELETE CASCADE）。
// ---------------------------------------------------------------------------
Problem ProblemService::create_problem(std::int64_t created_by,
                                       const ProblemWriteInput& in) {
    // 1) 业务校验
    Problem p = build_problem(in);          // 失败抛 BadRequest
    validate_cases(in.cases);                // 失败抛 BadRequest
    validate_tag_ids(in.tag_ids);            // 失败抛 BadRequest

    p.created_by = created_by;

    // 2) 写 problem 行
    std::int64_t new_id = 0;
    try {
        auto created = problems_->create(p);
        new_id = created.id;
    } catch (const std::exception& e) {
        spdlog::error("ProblemService::create_problem problems->create: {}", e.what());
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: create problem failed");
    }

    // 3) 写 tag 关联
    if (tags_ && !in.tag_ids.empty()) {
        try {
            tags_->set_problem_tags(new_id, in.tag_ids);
        } catch (...) {
            // 回滚：删 problem（testcases / problem_tags 走 CASCADE）
            try { problems_->soft_delete(new_id); } catch (...) {}
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::Internal,
                "internal error: set problem tags failed");
        }
    }

    // 4) 写 testcases
    if (testcases_ && !in.cases.empty()) {
        try {
            testcases_->create_many(new_id, in.cases);
        } catch (...) {
            // 回滚：删 problem（testcases 走 CASCADE）
            try { problems_->soft_delete(new_id); } catch (...) {}
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::Internal,
                "internal error: create testcases failed");
        }
    }

    spdlog::info("ProblemService::create_problem id={} title='{}' created_by={} cases={} tags={}",
                 new_id, p.title, created_by, in.cases.size(), in.tag_ids.size());

    // 5) 返回（含 id / created_at）
    try {
        auto fetched = problems_->find_by_id(new_id);
        if (!fetched.has_value()) {
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::Internal,
                "internal error: post-create fetch failed");
        }
        return *fetched;
    } catch (const oj::domain::AdminProblemError&) {
        throw;
    } catch (const std::exception& e) {
        spdlog::error("ProblemService::create_problem post-fetch: {}", e.what());
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: post-create fetch failed");
    }
}

// ---------------------------------------------------------------------------
//  update_problem
//
//  流程：业务校验 → find_by_id（不存在 → NotFound）→ update problem 行
//       → set_problem_tags → replace_by_problem
//  失败时 problem 行已被 update；不主动回滚（旧值已被覆盖），由 caller 决定
//  是否再调一次 update 修正（admin 编辑页 "保存" 是单一原子动作）。
// ---------------------------------------------------------------------------
void ProblemService::update_problem(std::int64_t id,
                                     const ProblemWriteInput& in) {
    // 1) 业务校验
    Problem p = build_problem(in);
    validate_cases(in.cases);
    validate_tag_ids(in.tag_ids);

    // 2) 存在性
    std::optional<Problem> existing;
    try {
        existing = problems_->find_by_id(id);
    } catch (const std::exception& e) {
        spdlog::error("ProblemService::update_problem find_by_id({}): {}", id, e.what());
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: find problem failed");
    }
    if (!existing.has_value()) {
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::NotFound,
            "problem not found: id=" + std::to_string(id));
    }

    // 3) 改 problem 行（不动 created_by / created_at）
    p.id         = id;
    p.created_by = existing->created_by;
    p.created_at = existing->created_at;
    try {
        problems_->update(p);
    } catch (const std::exception& e) {
        spdlog::error("ProblemService::update_problem update({}): {}", id, e.what());
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: update problem failed");
    }

    // 4) 改 tag 关联
    if (tags_) {
        try {
            tags_->set_problem_tags(id, in.tag_ids);
        } catch (const std::exception& e) {
            spdlog::error("ProblemService::update_problem set_problem_tags({}): {}", id, e.what());
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::Internal,
                "internal error: update problem tags failed");
        }
    }

    // 5) 改 testcases（全量替换）
    if (testcases_) {
        try {
            testcases_->replace_by_problem(id, in.cases);
        } catch (const std::exception& e) {
            spdlog::error("ProblemService::update_problem replace_by_problem({}): {}", id, e.what());
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::Internal,
                "internal error: update testcases failed");
        }
    }

    spdlog::info("ProblemService::update_problem id={} cases={} tags={}",
                 id, in.cases.size(), in.tag_ids.size());
}

// ---------------------------------------------------------------------------
//  delete_problem —— 软删 = 下架（is_published=0）；不动 submissions 关联
// ---------------------------------------------------------------------------
void ProblemService::delete_problem(std::int64_t id) {
    try {
        problems_->soft_delete(id);
    } catch (const std::exception& e) {
        // soft_delete 内部：找不到时抛 runtime_error；这里区分
        const std::string what = e.what();
        if (what.find("not found") != std::string::npos) {
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::NotFound,
                "problem not found: id=" + std::to_string(id));
        }
        spdlog::error("ProblemService::delete_problem({}): {}", id, what);
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: delete problem failed");
    }
    spdlog::info("ProblemService::delete_problem id={}", id);
}

// ---------------------------------------------------------------------------
//  set_published —— 上下架
// ---------------------------------------------------------------------------
void ProblemService::set_published(std::int64_t id, bool published) {
    try {
        problems_->set_published(id, published);
    } catch (const std::exception& e) {
        const std::string what = e.what();
        if (what.find("not found") != std::string::npos) {
            throw oj::domain::AdminProblemError(
                oj::domain::AdminProblemErrorKind::NotFound,
                "problem not found: id=" + std::to_string(id));
        }
        spdlog::error("ProblemService::set_published({}): {}", id, what);
        throw oj::domain::AdminProblemError(
            oj::domain::AdminProblemErrorKind::Internal,
            "internal error: set published failed");
    }
    spdlog::info("ProblemService::set_published id={} published={}", id, published);
}

}  // namespace oj::domain

