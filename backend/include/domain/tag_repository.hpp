#pragma once

// =============================================================================
//  oj::domain::ITagRepository —— 标签仓储接口
//  SPEC §4.2 tags / problem_tags 表；8 个预置标签 (不开放后台管理)
//
//  关键合约：
//    1. list_all 按 id ASC 返回全部 tag (业务上恒为 8 个)
//    2. tags_of_problem / set_problem_tags 管理 problem <-> tag 多对多
//    3. find_by_ids 批量取 tag (list 接口联表用)，找不到的 id 静默跳过
//    4. 任何 DB 错误抛 std::runtime_error
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "domain/problem_types.hpp"

namespace oj::domain {

class ITagRepository {
public:
    virtual ~ITagRepository() = default;

    /** 全部 tag (id ASC) —— 业务上恒为 8 个 */
    virtual std::vector<Tag> list_all() = 0;

    virtual std::optional<Tag> find_by_id(int id) = 0;
    virtual std::optional<Tag> find_by_slug(const std::string& slug) = 0;

    /**
     * 批量取 tag；按入参 ids 顺序返回。找不到的 id 静默跳过。
     * 用于 IProblemRepository::list 一次性 JOIN 出所有 tag
     */
    virtual std::vector<Tag> find_by_ids(const std::vector<int>& ids) = 0;

    /** 单题的全部 tag (按 id ASC) */
    virtual std::vector<Tag> tags_of_problem(std::int64_t problem_id) = 0;

    /** 单题 tag 的 id 列表 (按 id ASC) */
    virtual std::vector<int> tag_ids_of_problem(std::int64_t problem_id) = 0;

    /**
     * 全量替换 problem 的 tag 关联：先 DELETE problem_id 的旧关联，
     * 再按入参 ids 顺序插入新关联。事务内执行。
     */
    virtual void set_problem_tags(std::int64_t problem_id,
                                  const std::vector<int>& tag_ids) = 0;
};

}  // namespace oj::domain
