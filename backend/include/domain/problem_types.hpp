#pragma once

// =============================================================================
//  oj::domain::problem_types —— 题目/测试点/标签领域类型
//  SPEC §4.2 表结构 / §3.2.2 "ProblemService / SubmissionService"
//  本阶段只放"数据形状" —— 业务校验、CRUD 在 repo / service 层
//
//  设计要点：
//    1) enum 用 string 形式持久化（DB ENUM 字段）—— 提供 to_string / from_string
//    2) struct 全部 plain-old-data，可直接序列化（JSON / 业务层 DTO 转换）
//    3) ProblemListItem 不含 testcases（按需另查）—— 列表页 + 详情页分离
//    4) 时间字段统一用 std::string (ISO 8601) 透传 MySQL DATETIME，
//       业务层转 std::chrono::system_clock::time_point
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oj::domain {

// ---------------------------------------------------------------------------
//  Difficulty —— SPEC §2.2.1 (3 级)
// ---------------------------------------------------------------------------
enum class Difficulty { Easy, Medium, Hard };

std::string_view to_string(Difficulty d) noexcept;
std::optional<Difficulty> difficulty_from_string(std::string_view s) noexcept;

// ---------------------------------------------------------------------------
//  Language —— SPEC §4.2 submissions.language (5+1)
//  v1 只用 5 种；'cpp' 而不是 'c++' 是因为 MySQL ENUM 字段不允许 '+'
// ---------------------------------------------------------------------------
enum class Language { C, Cpp, Java, Python, Go };

std::string_view to_string(Language l) noexcept;
std::optional<Language> language_from_string(std::string_view s) noexcept;

// ---------------------------------------------------------------------------
//  Tag —— SPEC §4.2 (8 个预置, 不开放后台管理)
// ---------------------------------------------------------------------------
struct Tag {
    int         id{};
    std::string name;   // 中文: "数组" / "字符串" / ...
    std::string slug;   // 英文: "array" / "string" / ...
};

// ---------------------------------------------------------------------------
//  Testcase —— SPEC §4.2 (1-100 / 题; 总分 100)
// ---------------------------------------------------------------------------
struct Testcase {
    std::int64_t id{};            // 0 = 尚未插入 (新对象)
    std::int64_t problem_id{};    // 0 = 尚未关联
    int          case_index{};   // 1-based
    std::string  input;
    std::string  expected_output;
    bool         is_sample{false};
    int          score{};        // 0..100
};

// ---------------------------------------------------------------------------
//  Problem —— SPEC §4.2 (单条题目, 不含 testcases)
//  id == 0 表示尚未插入；时间字段由 repo 在 create 时填
// ---------------------------------------------------------------------------
struct Problem {
    std::int64_t id{};
    std::string  title;             // ≤100 字符
    std::string  content_md;        // ≤64KB, 业务层校验
    Difficulty   difficulty{Difficulty::Easy};
    int          time_limit_ms{2000};   // 1..10000
    int          memory_limit_mb{256};  // 64..1024
    int          output_limit_mb{64};   // 1..256
    bool         is_published{false};
    std::int64_t created_by{};     // user_id
    std::string  created_at;      // ISO 8601 (yyyy-MM-ddTHH:mm:ssZ)；create 后由 repo 写入
};

// ---------------------------------------------------------------------------
//  ProblemListItem —— 列表 / admin 后台列表用的轻量投影
//  包含每个题目的 tags (vector<Tag>) + 简单统计字段
//  (total/accepted) 给"通过率排序"用；统计为 0 时 sort 按 ID desc
// ---------------------------------------------------------------------------
struct ProblemListItem {
    std::int64_t   id{};
    std::string    title;
    Difficulty     difficulty{Difficulty::Easy};
    bool           is_published{false};
    std::int64_t   created_by{};
    std::string    created_at;
    std::vector<Tag> tags;

    // 判题统计 —— 由 repo 联表一次查出；为 0/0 表示该题尚无 finished submission
    int total_submissions{0};
    int accepted_submissions{0};

    // 通过率 = accepted / total (0..1)；stats 都为 0 时返回 0
    [[nodiscard]] double pass_rate() const noexcept {
        if (total_submissions <= 0) return 0.0;
        return static_cast<double>(accepted_submissions)
             / static_cast<double>(total_submissions);
    }
};

// ---------------------------------------------------------------------------
//  ProblemListQuery —— 列表查询参数
//  与 SPEC §3.3.5 G 题目列表 + §3.2.3 过滤契约对齐
// ---------------------------------------------------------------------------
struct ProblemListQuery {
    int page{1};                       // 1-based
    int page_size{20};                 // 10/20/50 (业务层校验)

    std::optional<Difficulty> difficulty;          // 难度过滤
    std::vector<std::string> tag_slugs;            // AND 语义："必须同时包含"  —— SPEC §3.3.5 G
    std::string              q;                    // 标题模糊搜索

    enum class Sort { IdDesc, CreatedDesc, PassRateDesc };
    Sort sort{Sort::IdDesc};                         // 排序字段

    bool include_unpublished{false};                // 游客只看已发布；admin 全看
};

// ---------------------------------------------------------------------------
//  ProblemListResult —— 列表结果 (含分页元信息)
// ---------------------------------------------------------------------------
struct ProblemListResult {
    std::vector<ProblemListItem> items;
    std::int64_t total{0};      // 命中条件的总行数 (用于分页器)
    int page{0};
    int page_size{0};
};

}  // namespace oj::domain
