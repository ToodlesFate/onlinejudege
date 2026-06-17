// =============================================================================
//  submission_service.cpp —— SubmissionService 实现
//  SPEC §2.3 / §5.2.3
// =============================================================================

#include "domain/submission_service.hpp"

#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

namespace oj::domain {

// ---------------------------------------------------------------------------
//  create
//
//  业务流程：
//    1) 字段校验（problem_id > 0；code 非空；code 长度 ≤ code_max_bytes）
//    2) 题目存在 + 已发布 —— 未发布题目对普通用户不可见，提交同样 404
//    3) repo->create → 返回新 id
//
//  错误分类：
//    - 字段错          → CreateSubmissionError::BadRequest (1001)
//    - 题目不存在/未发布 → CreateSubmissionError::NotFound   (1004)
//    - code 超长       → CreateSubmissionError::TooLarge   (1006)
//    - 其他 DB 错误    → CreateSubmissionError::Internal   (1007)
// ---------------------------------------------------------------------------
std::int64_t SubmissionService::create(std::int64_t user_id,
                                       std::int64_t problem_id,
                                       Language     language,
                                       std::string_view code) {
    (void)language;  // language 由 handler 解析；这里只透传

    // 1) 字段校验
    if (problem_id <= 0) {
        throw CreateSubmissionError(CreateSubmissionErrorKind::BadRequest,
                                    "problem_id must be a positive integer");
    }
    if (code.empty()) {
        throw CreateSubmissionError(CreateSubmissionErrorKind::BadRequest,
                                    "code is empty");
    }
    if (static_cast<int>(code.size()) > code_max_bytes_) {
        throw CreateSubmissionError(
            CreateSubmissionErrorKind::TooLarge,
            "code size " + std::to_string(code.size()) +
            " bytes exceeds limit " + std::to_string(code_max_bytes_));
    }

    // 2) 题目存在性 + 发布检查
    std::optional<Problem> p;
    try {
        p = problems_->find_by_id(problem_id);
    } catch (const std::exception& e) {
        spdlog::error("SubmissionService::create find_problem err: {}", e.what());
        throw CreateSubmissionError(CreateSubmissionErrorKind::Internal,
                                    "internal error");
    }
    if (!p.has_value() || !p->is_published) {
        // 与公开详情 404 语义对齐：未发布题目不存在
        throw CreateSubmissionError(CreateSubmissionErrorKind::NotFound,
                                    "problem not found");
    }

    // 3) 入库
    try {
        return submissions_->create(user_id, problem_id, language, code);
    } catch (const std::exception& e) {
        spdlog::error("SubmissionService::create repo err: {}", e.what());
        throw CreateSubmissionError(CreateSubmissionErrorKind::Internal,
                                    "internal error");
    }
}

// ---------------------------------------------------------------------------
//  get_detail
//
//  流程：
//    1) repo->get_full(id) → 找不到返回 nullopt
//    2) 可见性：AC 公开；其他仅 owner/admin
//    3) 样例点回填 input/expected_output
// ---------------------------------------------------------------------------
std::optional<SubmissionDetail> SubmissionService::get_detail(
    std::int64_t id,
    std::int64_t requester_id,
    bool         is_admin)
{
    // 1) 查 detail（含 username —— 由 repo JOIN 出来）
    std::optional<SubmissionDetail> detail;
    try {
        detail = submissions_->get_full(id);
    } catch (const std::exception& e) {
        spdlog::error("SubmissionService::get_full err: {}", e.what());
        throw GetSubmissionError(GetSubmissionErrorKind::Internal, "internal error");
    }
    if (!detail.has_value()) {
        return std::nullopt;
    }

    // 2) 可见性 —— SPEC §3.3.5 K："公开访问：仅当 result=AC 或登录后是本人时可看"
    const Submission& s = detail->submission;
    const bool is_ac =
        (s.status == SubmissionStatus::Finished) &&
        s.result.has_value() &&
        (*s.result == SubmissionResult::AC);
    const bool is_owner = (requester_id != 0 && s.user_id == requester_id);

    if (!is_ac && !is_owner && !is_admin) {
        throw GetSubmissionError(GetSubmissionErrorKind::Forbidden,
                                  "submission not visible to this user");
    }

    // 3) 样例点回填 input/expected_output
    //    一次拉所有样例点 → 哈希到 case_index；非样例点保持空
    if (testcases_) {
        try {
            auto samples = testcases_->list_samples(s.problem_id);
            std::map<int, const Testcase*> by_index;
            for (const auto& tc : samples) {
                by_index[tc.case_index] = &tc;
            }
            for (auto& c : detail->cases) {
                auto it = by_index.find(c.case_index);
                if (it != by_index.end()) {
                    c.input           = it->second->input;
                    c.expected_output = it->second->expected_output;
                }
            }
        } catch (const std::exception& e) {
            // 失败也不影响主流程：detail 中 input/expected 留空即可
            spdlog::warn("SubmissionService::get_detail list_samples err: {}", e.what());
        }
    }

    return detail;
}

}  // namespace oj::domain
