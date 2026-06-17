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

}  // namespace oj::domain

