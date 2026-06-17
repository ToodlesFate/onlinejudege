#pragma once

// =============================================================================
//  oj::domain::ITestcaseRepository —— 测试点仓储接口
//  SPEC §4.2 testcases 表；1 个 problem 可挂 1..100 个 case
//
//  关键合约：
//    1. list_by_problem 返回完整测试点列表 (按 case_index 升序)
//    2. list_samples 返回 is_sample=1 的子集（题目详情页用）
//    3. create_many / replace_by_problem 都要求"事务 + 按 problem_id 关联"
//       单独失败时回滚；case_index 必须唯一 (DB UNIQUE 约束兜底)
//    4. 任何 DB 错误抛 std::runtime_error
// =============================================================================

#include <cstdint>
#include <vector>

#include "domain/problem_types.hpp"

namespace oj::domain {

class ITestcaseRepository {
public:
    virtual ~ITestcaseRepository() = default;

    /** 全部测试点（按 case_index ASC） */
    virtual std::vector<Testcase> list_by_problem(std::int64_t problem_id) = 0;

    /** 仅返回 is_sample=1 的子集；按 case_index ASC */
    virtual std::vector<Testcase> list_samples(std::int64_t problem_id) = 0;

    /**
     * 批量插入。problem_id 由入参指定（调用方已 create() 拿到 id）。
     * 入参 cases 的 case_index 必须在 [1, 100] 且互不重复；越界由 repo 抛
     * runtime_error。
     */
    virtual void create_many(std::int64_t problem_id,
                             const std::vector<Testcase>& cases) = 0;

    /**
     * 全量替换：先删该 problem_id 下所有 case，再按入参顺序插入。
     * admin "编辑题目" 流程（PUT /api/admin/problems/{id}）用本接口。
     */
    virtual void replace_by_problem(std::int64_t problem_id,
                                    const std::vector<Testcase>& cases) = 0;

    /** 删除某 problem 全部 testcases（problem 删除场景的级联兜底） */
    virtual void delete_by_problem(std::int64_t problem_id) = 0;
};

}  // namespace oj::domain
