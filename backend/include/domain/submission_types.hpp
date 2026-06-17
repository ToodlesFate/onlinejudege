#pragma once

// =============================================================================
//  oj::domain::submission_types —— 提交 / 测试点结果领域类型
//  SPEC §2.3.2 判题状态机 + §4.2 submissions / submission_cases 表
//
//  与 problem_types 分开放是因为：
//    1) Submission / SubmissionCase 用 enum 比较多（status / result / language）
//    2) SubmissionCase 跟 JudgeResult 的 CaseResult 字段大致一致但生命周期不同
//       (host 端持久化的是 ENUM 字符串)
//    3) ISubmissionRepository 接口放独立头文件，避免 problem_types.hpp 改动
//       把整个 domain 编译都重拉一次
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/problem_types.hpp"  // 用 Language / Difficulty

namespace oj::domain {

// ---------------------------------------------------------------------------
//  SubmissionStatus —— 状态机 4 态 (SPEC §2.3.2)
//    queued → compiling → running → finished (one of 8 terminal states)
// ---------------------------------------------------------------------------
enum class SubmissionStatus { Queued, Compiling, Running, Finished };

std::string_view to_string(SubmissionStatus s) noexcept;
std::optional<SubmissionStatus> submission_status_from_string(std::string_view s) noexcept;

/**
 * 主流程状态机是否"已结束"（SPEC §2.3.2 状态机）。
 * 只有 status == Finished 算"已结束"；其余 3 态（queued/compiling/running）
 * 仍在前台轮询。
 *
 * @note  与 SubmissionResult 无关：result 是 status=Finished 之后的填充字段。
 *        这里只判主流程 status。
 */
bool is_terminal_status(SubmissionStatus s) noexcept;

// ---------------------------------------------------------------------------
//  SubmissionResult —— 8 态结果 (与 DockerClient::JudgeStatus 对齐)
//  这里再次定义为 domain 类型而不是直接用 infra 的 JudgeStatus，是为了
//  保持 Http → Domain ← Infra 的依赖方向：Domain 层不应依赖 Infra 的枚举。
// ---------------------------------------------------------------------------
enum class SubmissionResult {
    AC,   // Accepted
    WA,   // Wrong Answer
    TLE,  // Time Limit Exceeded
    MLE,  // Memory Limit Exceeded
    OLE,  // Output Limit Exceeded
    RE,   // Runtime Error
    CE,   // Compile Error
    SE,   // System Error
};

std::string_view to_string(SubmissionResult r) noexcept;
std::optional<SubmissionResult> submission_result_from_string(std::string_view s) noexcept;

/**
 * 8 种结果态全部是"已结束"（SPEC §2.3.2）。一旦 status=Finished，result 必填
 * 8 态之一，函数返回 true。
 */
bool is_terminal(SubmissionResult /*r*/) noexcept;

/**
 * 是否"早退出"—— 即没有真正进入 running 阶段、没产生任何测试点结果。
 * 早退出原因：编译失败 (CE) 或判题机系统级异常 (SE)。
 * 这两类在 SPEC §2.3.2 状态机中由 compiling 直接 → finished，绕过 running。
 *
 * 用于：
 *  - 提交详情页的"测试点表格"空态文案（CE/SE 不显示 0 个测试点）
 *  - 前端状态机可视化的 running 节点标为 'skipped'
 *  - judge dispatcher 决定是否要写 submission_cases
 */
bool is_early_exit(SubmissionResult r) noexcept;

// ---------------------------------------------------------------------------
//  Submission —— SPEC §4.2 submissions 表
//  id == 0 表示尚未插入；created_at / finished_at 由 repo 在 create / finish 时填
// ---------------------------------------------------------------------------
struct Submission {
    std::int64_t  id{};
    std::int64_t  user_id{};
    std::int64_t  problem_id{};
    Language      language{Language::Cpp};
    std::string   code;
    SubmissionStatus status{SubmissionStatus::Queued};
    std::optional<SubmissionResult> result;       // 仅 status=Finished 时有值
    int           total_score{0};
    int           time_used_ms{0};
    int           memory_used_kb{0};
    std::string   compile_output;                 // CE 时填；MEDIUMTEXT
    std::string   judge_message;                  // SE 时填；≤ 500 字符
    std::string   created_at;                     // ISO 8601
    std::string   finished_at;                    // ISO 8601；status!=Finished 时为空
};

// ---------------------------------------------------------------------------
//  SubmissionCase —— SPEC §4.2 submission_cases 表
//  注意点：
//    - user_output 仅 is_sample=1 时存储；host 端决定
//    - input / expected_output 由 service 层在 get_detail 时从 testcases 表回填，
//      只对 is_sample=1 的样例点填充；隐藏点始终为空。这样 API 响应可以一次
//      返回前端需要的全部信息（user_output / expected_output / 隐藏点状态）
// ---------------------------------------------------------------------------
struct SubmissionCase {
    std::int64_t id{};
    std::int64_t submission_id{};
    int          case_index{};
    SubmissionResult status{SubmissionResult::AC};
    int          time_used_ms{0};
    int          memory_used_kb{0};
    int          score{0};
    bool         is_sample{false};
    std::string  user_output;       // 仅 is_sample=1 时填；其余置空
    std::string  input;             // 仅 is_sample=1 时由 service 回填
    std::string  expected_output;   // 仅 is_sample=1 时由 service 回填
};

// ---------------------------------------------------------------------------
//  ClaimedTask —— claim_one() 返回的"刚抢到的最小任务集"
//  只有 dispatcher 真正用得到：拿到 id + language + code + problem_id 后
//  再调 load_task(id) 拿完整数据（limits / testcases）。
// ---------------------------------------------------------------------------
struct ClaimedTask {
    std::int64_t submission_id{};
    std::int64_t problem_id{};
    Language     language{Language::Cpp};
    std::string  code;
    std::string  created_at;   // ISO 8601；仅做日志用
};

// ---------------------------------------------------------------------------
//  JudgeTaskPayload —— dispatcher 拉到一个 submission 后的"运行所需数据"
//  等价于 Submission + 题目限制 + 测试点列表。
//  ISubmissionRepository::load_task(id) 返回此结构。
// ---------------------------------------------------------------------------
struct JudgeTaskPayload {
    std::int64_t submission_id{};
    std::int64_t problem_id{};
    Language     language{Language::Cpp};
    std::string  code;
    int          time_limit_ms{2000};
    int          memory_limit_mb{256};
    int          output_limit_mb{64};
    std::vector<std::pair<std::string, std::string>> testcases;  // (input, expected)
};

// ---------------------------------------------------------------------------
//  SubmissionDetail —— get_full() 返回：submission 本体 + 提交人 username + cases 列表
//  username 由 repo 在 get_full 时通过 LEFT JOIN users 表填充；
//  列表视图（list_by_user / list_public_accepted）不需要 username。
// ---------------------------------------------------------------------------
struct SubmissionDetail {
    Submission                  submission;
    std::string                 username;       // 提交人 username
    std::vector<SubmissionCase> cases;
};

// ---------------------------------------------------------------------------
//  SubmissionListQuery / SubmissionListResult —— list 查询
// ---------------------------------------------------------------------------
struct SubmissionListQuery {
    int page{1};
    int page_size{20};
    std::int64_t user_id{0};        // 0 = 忽略（list_public_accepted 不需要）
    std::int64_t problem_id{0};     // 0 = 忽略
    std::optional<Language> language;
    std::optional<SubmissionStatus> status;  // list_by_user 用
};

struct SubmissionListResult {
    std::vector<Submission> items;
    std::int64_t total{0};
    int page{0};
    int page_size{0};
};

}  // namespace oj::domain