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

private:
    std::shared_ptr<IProblemRepository>   problems_;
    std::shared_ptr<ITestcaseRepository> testcases_;
    std::shared_ptr<ITagRepository>       tags_;
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

