#pragma once

// =============================================================================
//  oj::domain::ISubmissionService —— 提交领域服务
//  SPEC §2.3 / §5.2.3 POST /api/submissions + GET /api/submissions/{id}
//
//  业务规则（与 SPEC 对齐）：
//    1. create():
//       - 字段校验：problem_id > 0；language 已由 handler 解析为 enum；
//         code 非空且 ≤ code_max_bytes（默认 65536）
//       - 题目必须存在且已发布（is_published=1）
//       - 入库（status=queued）；返回新 id
//       - 抛 CreateSubmissionError 表示业务错误，kind 决定 handler 映射 ErrorCode
//
//    2. get_detail():
//       - 查不到 → nullopt（handler 翻译 1004）
//       - 可见性：AC 公开；非 AC / 未完成 → 仅本人 / admin
//       - 不可见 → 抛 GetSubmissionError(Forbidden)
//       - 样例点：填充 input/expected_output（从 testcases 表）；隐藏点保持空
//
//  设计要点：
//    - 依赖 IProblemRepository + ITestcaseRepository + ISubmissionRepository，
//      全部 domain 层接口（依赖倒置）；测试时可注入 in-memory 实现
//    - code_max_bytes 由构造期注入，避免在 service 里硬编码 SPEC 默认值
// =============================================================================

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "domain/problem_repository.hpp"
#include "domain/submission_repository.hpp"
#include "domain/submission_types.hpp"
#include "domain/testcase_repository.hpp"

namespace oj::domain {

// ---------------------------------------------------------------------------
//  CreateSubmissionError —— POST /api/submissions 业务错误
// ---------------------------------------------------------------------------
enum class CreateSubmissionErrorKind {
    BadRequest,    // → 1001  (problem_id 非法 / code 为空)
    NotFound,      // → 1004  (题目不存在 / 未发布)
    TooLarge,      // → 1006  (code > 64KB)
    Internal,      // → 1007
};

class CreateSubmissionError : public std::runtime_error {
public:
    CreateSubmissionError(CreateSubmissionErrorKind k, std::string msg)
        : std::runtime_error(std::move(msg)), kind_(k) {}
    [[nodiscard]] CreateSubmissionErrorKind kind() const noexcept { return kind_; }
private:
    CreateSubmissionErrorKind kind_;
};

// ---------------------------------------------------------------------------
//  GetSubmissionError —— GET /api/submissions/{id} 业务错误
// ---------------------------------------------------------------------------
enum class GetSubmissionErrorKind {
    Forbidden,     // → 1003  (非 AC 非 owner 非 admin)
    Internal,      // → 1007
};

class GetSubmissionError : public std::runtime_error {
public:
    GetSubmissionError(GetSubmissionErrorKind k, std::string msg)
        : std::runtime_error(std::move(msg)), kind_(k) {}
    [[nodiscard]] GetSubmissionErrorKind kind() const noexcept { return kind_; }
private:
    GetSubmissionErrorKind kind_;
};

// ---------------------------------------------------------------------------
//  ISubmissionService
// ---------------------------------------------------------------------------
class ISubmissionService {
public:
    virtual ~ISubmissionService() = default;

    /**
     * POST /api/submissions 主流程：
     *   1) 字段校验（problem_id > 0；code 非空；|code| ≤ code_max_bytes）
     *   2) 题目存在 + 已发布
     *   3) repo->create（status=queued）→ 返回新 id
     *
     * 抛 CreateSubmissionError；DB 错误归类为 Internal。
     */
    virtual std::int64_t create(std::int64_t user_id,
                                 std::int64_t problem_id,
                                 Language language,
                                 std::string_view code) = 0;

    /**
     * GET /api/submissions/{id} 主流程：
     *   1) repo->get_full(id) → nullopt 表示 404
     *   2) 可见性：AC 公开；其他情况仅 owner/admin
     *   3) 样例点回填 input/expected_output
     *
     * @param requester_id 0 = 匿名访问
     * @param is_admin    是否 admin（仅在 authenticated=true 时有效）
     * @return nullopt 表示 404；抛 GetSubmissionError 表示 403
     */
    virtual std::optional<SubmissionDetail> get_detail(
        std::int64_t id,
        std::int64_t requester_id,
        bool is_admin) = 0;
};

// ---------------------------------------------------------------------------
//  SubmissionService —— 标准实现
// ---------------------------------------------------------------------------
class SubmissionService final : public ISubmissionService {
public:
    SubmissionService(std::shared_ptr<ISubmissionRepository>   submissions,
                      std::shared_ptr<IProblemRepository>     problems,
                      std::shared_ptr<ITestcaseRepository>    testcases,
                      int                                     code_max_bytes)
        : submissions_(std::move(submissions)),
          problems_(std::move(problems)),
          testcases_(std::move(testcases)),
          code_max_bytes_(code_max_bytes) {}

    std::int64_t create(std::int64_t user_id,
                        std::int64_t problem_id,
                        Language     language,
                        std::string_view code) override;

    std::optional<SubmissionDetail> get_detail(
        std::int64_t id,
        std::int64_t requester_id,
        bool         is_admin) override;

    int code_max_bytes() const noexcept { return code_max_bytes_; }

private:
    std::shared_ptr<ISubmissionRepository>   submissions_;
    std::shared_ptr<IProblemRepository>     problems_;
    std::shared_ptr<ITestcaseRepository>    testcases_;
    int                                     code_max_bytes_;
};

}  // namespace oj::domain
