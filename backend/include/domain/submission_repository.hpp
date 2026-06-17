#pragma once

// =============================================================================
//  oj::domain::ISubmissionRepository —— 提交仓储接口
//  SPEC §3.2.2 "SubmissionRepo (MySQL)" + §2.3.2 / §4.2
//
//  关键合约：
//    1. create() — INSERT 一行 submission，status=queued，返回 id
//    2. claim_one() — 抢一个 queued 任务：
//         SELECT ... FROM submissions WHERE status='queued' ORDER BY created_at
//         LIMIT 1 FOR UPDATE SKIP LOCKED
//       然后把 status 改为 'running'（简化：把 'compiling' 与 'running' 合并为
//       'running'，与 SPEC §2.3.2 文档展示的状态机兼容；测试 / 验收统一
//       走 'running'）。
//       抢到 → out 字段填齐 + return true
//       没抢到 → return false（正常）
//       异常 → throw std::runtime_error
//    3. load_task(id) — dispatcher 拿到 id 后，再去拉测试点 + 题目限制，
//       组装成 JudgeTaskPayload 返回。
//    4. update_status(id, new_status) — 改 status；finished 时 finished_at 自动写
//    5. finish(id, result, ...) — 一次性写完所有终态字段
//    6. insert_case(submission_id, case) — 逐 case 插入 submission_cases 行
//    7. mark_all_running_as_se_on_shutdown(reason) — graceful shutdown 兜底
//    8. find_by_id / get_full / list_by_user / list_public_accepted — handler 用
//    9. 任何 DB 错误抛 std::runtime_error
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "domain/submission_types.hpp"

namespace oj::domain {

class ISubmissionRepository {
public:
    virtual ~ISubmissionRepository() = default;

    /** INSERT 一条 queued 记录；返回新 id */
    virtual std::int64_t create(std::int64_t user_id,
                                 std::int64_t problem_id,
                                 Language language,
                                 std::string_view code) = 0;

    /**
     * 抢一个 queued 任务：原子地 SELECT 一行 → status='running' → 返回。
     * 用 `FOR UPDATE SKIP LOCKED` (MySQL 8.0+) 让多 worker 并发抢任务不互锁。
     *
     * @return true  表示抢到任务；out 字段填齐
     *         false 表示当前没有 queued 任务（正常）
     *         throw  表示 DB 错误
     */
    virtual bool claim_one(ClaimedTask& out) = 0;

    /**
     * 加载完整任务负载：submission 行 + problem 限制 + testcases 列表。
     * 找不到对应 submission / problem → throw std::runtime_error
     */
    virtual JudgeTaskPayload load_task(std::int64_t submission_id) = 0;

    /** 状态机更新；status='finished' 时 finished_at 自动写 */
    virtual void update_status(std::int64_t submission_id,
                               SubmissionStatus new_status) = 0;

    /**
     * 一次性写完终态所有字段。status='finished'，finished_at=NOW()，
     * compile_output / judge_message 一起填。
     */
    virtual void finish(std::int64_t submission_id,
                        SubmissionResult result,
                        int total_score,
                        int time_used_ms,
                        int memory_used_kb,
                        std::string_view compile_output,
                        std::string_view judge_message) = 0;

    /** 插入一条 submission_cases 行 */
    virtual void insert_case(std::int64_t submission_id,
                             const SubmissionCase& c) = 0;

    /**
     * Graceful shutdown 时：把所有 status='running' 的 submission 改成
     * status='finished' + result='SE' + judge_message='<reason>'。
     */
    virtual void mark_all_running_as_se_on_shutdown(std::string_view reason) = 0;

    /** 按 id 查 submission 本体（不含 cases） */
    virtual std::optional<Submission> find_by_id(std::int64_t id) = 0;

    /** get_full：返回 submission + cases（按 case_index ASC） */
    virtual std::optional<SubmissionDetail> get_full(std::int64_t id) = 0;

    /** list_by_user：按 user_id + 可选过滤 + 分页 + 按 created_at DESC */
    virtual SubmissionListResult list_by_user(const SubmissionListQuery& q) = 0;

    /** list_public_accepted：仅 result=AC；同 list_by_user 接口签名 */
    virtual SubmissionListResult list_public_accepted(const SubmissionListQuery& q) = 0;
};

}  // namespace oj::domain