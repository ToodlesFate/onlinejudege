#pragma once

// =============================================================================
//  oj::domain::IProblemService —— 题目领域服务接口
//  SPEC §3.2.2 "ProblemService" / §3.3.5 G 列表契约 / §5.2.2 GET /api/problems
//
//  设计要点：
//    1) 列表参数校验集中在这里（handler 只做 URL→DTO 解析，service 做业务校验）
//    2) 业务规则与持久化解耦：service 只依赖 IProblemRepository 接口
//    3) CRUD (create/update/delete) 在本阶段不实现 —— 后续 admin API 阶段补
// =============================================================================

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "domain/problem_repository.hpp"
#include "domain/problem_types.hpp"
#include "domain/tag_repository.hpp"
#include "domain/testcase_repository.hpp"

namespace oj::domain {

// 允许的 page_size —— SPEC §2.2.2（10/20/50）
inline constexpr int kProblemPageSizeOptions[] = {10, 20, 50};
inline constexpr int kProblemDefaultPageSize   = 20;
inline constexpr int kProblemMaxPageSize       = 50;

// 题目详情 DTO —— 列表项 + 关联 tags + 样例测试点
//  注意：测试点列表**只**包含 is_sample=1 的（隐藏点不返回 —— SPEC §2.3.2 / §3.3.5 K）
struct ProblemDetail {
    Problem             problem;
    std::vector<Tag>    tags;
    std::vector<Testcase> sample_testcases;
};

// 后台详情 DTO —— 题面 + tags + 全部测试点（包含隐藏点；admin 编辑用）
struct AdminProblemDetail {
    Problem               problem;
    std::vector<Tag>      tags;
    std::vector<Testcase> testcases;   // 全部测试点（按 case_index ASC）
};

// 业务校验约束 —— SPEC §2.2.1
inline constexpr std::size_t kProblemTitleMax        = 100;
inline constexpr std::size_t kProblemContentMdMax    = 65536;  // 64KB
inline constexpr int         kProblemTimeLimitMin    = 1;
inline constexpr int         kProblemTimeLimitMax    = 10000;
inline constexpr int         kProblemMemoryLimitMin  = 64;
inline constexpr int         kProblemMemoryLimitMax  = 1024;
inline constexpr int         kProblemOutputLimitMin  = 1;
inline constexpr int         kProblemOutputLimitMax  = 256;
inline constexpr int         kProblemCaseMin         = 1;
inline constexpr int         kProblemCaseMax         = 100;
inline constexpr int         kProblemCaseScoreSum    = 100;

// 后台管理错误类型 —— service 层抛，handler 层翻译为 ErrorCode
enum class AdminProblemErrorKind {
    BadRequest,        // 字段缺失 / 越界 / 校验失败
    NotFound,          // 题目不存在
    Internal,          // DB 错误
};

class AdminProblemError : public std::runtime_error {
public:
    AdminProblemError(AdminProblemErrorKind k, const std::string& msg)
        : std::runtime_error(msg), kind_(k) {}
    [[nodiscard]] AdminProblemErrorKind kind() const noexcept { return kind_; }
private:
    AdminProblemErrorKind kind_;
};

// admin 写操作 DTO —— POST/PUT 复用同一形状
//
//  字段命名与 SPEC §5.2.4 "POST /api/admin/problems body" 一致：
//    title / content_md / difficulty / time_limit_ms / memory_limit_mb /
//    output_limit_mb / is_published / tag_ids[] / cases[]
//
//  校验规则（service 层集中，handler 不重复）：
//    - title: 1..100 字符，trim 后不能为空
//    - content_md: 1..65536 字节
//    - difficulty: 必填，且 ∈ {easy, medium, hard}
//    - time_limit_ms / memory_limit_mb / output_limit_mb: SPEC §2.2.1 区间
//    - tag_ids: 可空数组（admin 可不改 tag）；若非空，每个 id 都必须在 tags 表内
//    - cases: 1..100 个；case_index 必填、唯一、∈ [1,100]；score ∈ [0,100]；
//             所有 case 的 score 之和必须等于 100
struct ProblemWriteInput {
    std::string                title;
    std::string                content_md;
    std::string                difficulty_str;   // "easy"/"medium"/"hard"
    int                        time_limit_ms{2000};
    int                        memory_limit_mb{256};
    int                        output_limit_mb{64};
    bool                       is_published{false};
    std::vector<int>           tag_ids;          // 空数组 = 不关联任何 tag
    std::vector<Testcase>      cases;            // 1..100 个；case_index 必填且唯一
};

class IProblemService {
public:
    virtual ~IProblemService() = default;

    /**
     * 列表查询 —— 业务校验：
     *   - page < 1 → 1
     *   - page_size 不在 {10,20,50} → kProblemDefaultPageSize
     *   - difficulty 字符串解析失败 → 忽略（不筛）
     *   - sort 字符串解析失败 → IdDesc
     *   - include_unpublished 默认 false（公开 /api/problems 永远只返回已发布）
     *   - tag_slugs 保留入参顺序，service 层不做 dedup（DB IN 查重 O(n) 即可）
     */
    virtual ProblemListResult list(const ProblemListQuery& q) = 0;

    /**
     * 详情查询 —— SPEC §5.2.2 GET /api/problems/{id}：
     *   - 找不到 / include_unpublished=false 但 is_published=false → std::nullopt
     *     （handler 翻译为 1004 NotFound）
     *   - 找到时返回 Problem + tags + 仅 is_sample=1 的样例
     */
    virtual std::optional<ProblemDetail> get_detail(std::int64_t id,
                                                   bool include_unpublished) = 0;

    /**
     * 列出全部 tag —— SPEC §5.2.2 GET /api/tags
     *   - 公开 API，返回 8 个预置 tag（id ASC）
     *   - 业务上不会变，但 repo 每次都重查（8 行，索引 PK，开销忽略）
     *   - 后续 phase 可以加内存缓存 + TTL，目前不做
     */
    virtual std::vector<Tag> list_tags() = 0;

    // ----- 后台管理（SPEC §5.2.4 admin API） ---------------------------

    /**
     * 后台详情（含全部测试点）—— SPEC §3.3.5 M GET /api/admin/problems/{id}/edit-data
     *   - 找不到 → std::nullopt（handler 翻译 1004）
     *   - 找到时返回 Problem + tags + 全部 testcases（按 case_index ASC）
     */
    virtual std::optional<AdminProblemDetail> get_admin_detail(std::int64_t id) = 0;

    /**
     * 后台创建 —— SPEC §5.2.4 POST /api/admin/problems
     *   - 字段校验（title / content_md / difficulty / limits / cases / tag_ids）
     *   - 事务内：create problem → set_problem_tags → replace_by_problem
     *   - 任一失败 → 抛 AdminProblemError(BadRequest) 或 Internal
     *   - 返回新建题目（含 id / created_at）
     */
    virtual Problem create_problem(std::int64_t created_by,
                                   const ProblemWriteInput& in) = 0;

    /**
     * 后台全量更新 —— SPEC §5.2.4 PUT /api/admin/problems/{id}
     *   - 与 create_problem 走相同的校验
     *   - 不存在 → AdminProblemError(NotFound)
     *   - 事务内：update problem → set_problem_tags → replace_by_problem
     */
    virtual void update_problem(std::int64_t id,
                                const ProblemWriteInput& in) = 0;

    /**
     * 后台软删 —— SPEC §5.2.4 DELETE /api/admin/problems/{id}
     *   - is_published=0（不真删 —— 保留提交历史，SPEC §2.2.1）
     *   - 不存在 → AdminProblemError(NotFound)
     */
    virtual void delete_problem(std::int64_t id) = 0;

    /**
     * 后台上下架 —— SPEC §5.2.4 PATCH /api/admin/problems/{id}/publish
     *   - is_published = published
     *   - 不存在 → AdminProblemError(NotFound)
     */
    virtual void set_published(std::int64_t id, bool published) = 0;

#ifndef OJ_PRODUCTION
    // 测试辅助：让 handler 测试可以直接拿到 tags repo
    // （不在生产代码中暴露该 API）
    virtual std::shared_ptr<ITagRepository> tags_repo_for_test() = 0;
#endif
};

// 标准实现 —— 直接代理到 IProblemRepository
//   v1 业务层没什么逻辑（CRUD/缓存都在后续 admin 阶段补），但参数校验
//   在这里集中，handler 只负责 URL → ProblemListQuery 的转换
class ProblemService final : public IProblemService {
public:
    ProblemService(std::shared_ptr<IProblemRepository>    problems,
                   std::shared_ptr<ITestcaseRepository>  testcases,
                   std::shared_ptr<ITagRepository>        tags)
        : problems_(std::move(problems)),
          testcases_(std::move(testcases)),
          tags_(std::move(tags)) {}

    ProblemListResult list(const ProblemListQuery& q) override;
    std::optional<ProblemDetail> get_detail(std::int64_t id,
                                           bool include_unpublished) override;
    std::vector<Tag> list_tags() override;

    std::optional<AdminProblemDetail> get_admin_detail(std::int64_t id) override;
    Problem create_problem(std::int64_t created_by,
                           const ProblemWriteInput& in) override;
    void update_problem(std::int64_t id,
                        const ProblemWriteInput& in) override;
    void delete_problem(std::int64_t id) override;
    void set_published(std::int64_t id, bool published) override;
#ifndef OJ_PRODUCTION
    std::shared_ptr<ITagRepository> tags_repo_for_test() override { return tags_; }
#endif

private:
    std::shared_ptr<IProblemRepository>   problems_;
    std::shared_ptr<ITestcaseRepository> testcases_;
    std::shared_ptr<ITagRepository>       tags_;

    // 内部：把 ProblemWriteInput 翻译成 Problem + 校验 cases 字段
    // 失败抛 AdminProblemError(BadRequest, msg)
    Problem build_problem(const ProblemWriteInput& in) const;
    void    validate_cases(const std::vector<Testcase>& cases);
    void    validate_tag_ids(const std::vector<int>& tag_ids);
};

//
//  URL query 解析工具 —— handler 用
//  把 httplib::Request::params (multimap<string,string>) 翻译成 ProblemListQuery
//
//  设计：单独一个自由函数而非 class 成员，因为 service 不应知道 HTTP 层细节。
//  handler 负责调用 → 把结果传给 service.list() → 拿 ProblemListResult → 序列化为 JSON。
//
//  SPEC §5.2.2 GET /api/problems 的 query 形态：
//      ?page=1&size=20&difficulty=easy&tag=dp&tag=array&sort=created_desc&q=hash
//
//  tag 支持两种形态：
//    1) 多次出现：   ?tag=dp&tag=array        （httplib::Request::params 是 multimap）
//    2) 逗号分隔：   ?tag=dp,array
//  业务上等价 —— "必须同时拥有" 语义。
//
struct ParsedListQuery {
    ProblemListQuery query;
    std::string      error_message;  // 空 = 解析成功；非空 = 客户端输入有问题
};

// 从 URL 原始参数构造 ProblemListQuery；同时做参数校验。
//  is_admin=true 时（admin API 路径）include_unpublished 才被允许为 true
ParsedListQuery parse_problems_list_query(
    const std::multimap<std::string, std::string>& params,
    bool is_admin = false);

}  // namespace oj::domain

