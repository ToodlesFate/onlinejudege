#pragma once

// =============================================================================
//  oj::domain::IProblemRepository —— 题目仓储接口
//  SPEC §3.2.2 "ProblemRepo (MySQL)" + §3.2.1 三层依赖
//  Domain 只依赖本接口；具体实现 (Mysql / InMemory) 由 Infra 提供
//
//  关键合约：
//    1. find_by_id 返回单条 Problem (不含 testcases / tags) ——
//       列表/详情分离，避免大字段 (MEDIUMTEXT) 拉满 IO
//    2. list 走 ProblemListQuery；不传过滤条件 = 全部 + 排序 + 分页
//    3. create / update 必须事务内执行：Problem 行 + tag 关联一起写
//    4. soft_delete(id) → is_published=0；不真删 (保留提交历史)
//    5. 任何 DB 错误抛 std::runtime_error
// =============================================================================

#include <cstdint>
#include <optional>

#include "domain/problem_types.hpp"

namespace oj::domain {

class IProblemRepository {
public:
    virtual ~IProblemRepository() = default;

    /** 按 ID 查题目本体；找不到 → nullopt。**不含** testcases / tags。 */
    virtual std::optional<Problem> find_by_id(std::int64_t id) = 0;

    /**
     * 列表查询；分页由 page/page_size 决定 (1-based)。
     *  - difficulty 过滤按字段匹配
     *  - tag_slugs 多选 AND 语义（题目必须同时拥有所有这些 tag）
     *  - q 标题模糊搜索 (LIKE %q%)
     *  - include_unpublished=false 时只返回 is_published=1
     *  - sort 指定排序方式
     */
    virtual ProblemListResult list(const ProblemListQuery& q) = 0;

    /**
     * 插入新题；id / created_at 由 repo 写入并回填到返回的 Problem。
     * 注意：tag 关联不通过本接口写；调用方应配套调用 ITagRepository::set_problem_tags
     */
    virtual Problem create(const Problem& p) = 0;

    /**
     * 全量更新 (PUT 语义)；id 必填；不存在 → 抛 std::runtime_error
     */
    virtual void update(const Problem& p) = 0;

    /** 软删：is_published=0；不影响 submissions 关联 */
    virtual void soft_delete(std::int64_t id) = 0;

    /** 上下架 (admin 操作)；不存在 → 抛 std::runtime_error */
    virtual void set_published(std::int64_t id, bool published) = 0;

    /**
     * 单题判题统计 (finished submissions)：返回 {total, accepted}。
     * 无任何 submission 时返回 {0, 0}。
     * 实现可走 SELECT COUNT + SUM(CASE)；list() 内部可批量调用此方法。
     */
    virtual std::pair<int, int> submission_stats(std::int64_t problem_id) = 0;
};

}  // namespace oj::domain
