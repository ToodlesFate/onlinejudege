// =============================================================================
//  test_problem_service.cpp —— ProblemService 单元测试
//  不依赖 MySQL；用 InMemoryProblemRepository 模拟 IProblemRepository
//
//  覆盖：
//    1) URL query 解析（page / size / difficulty / sort / tag / q / include_unpublished）
//    2) page_size 白名单（10/20/50）—— 非法值走默认 20
//    3) page < 1 → 1
//    4) tag 多次出现 / 逗号分隔 / dedup
//    5) 公共 API 强制 include_unpublished=false；admin 路径尊重入参
//    6) 业务校验 → service.list() 通过 repo 拿到正确数据
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"

namespace {

using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::parse_problems_list_query;
using oj::domain::Problem;
using oj::domain::ProblemListItem;
using oj::domain::ProblemListQuery;
using oj::domain::ProblemListResult;
using oj::domain::ProblemService;
using oj::domain::Tag;

// ---------------------------------------------------------------------------
//  InMemoryProblemRepository —— 测试用 mock
//  简单实现：内存里存 vector，支持 list/find_by_id；不实现 create/update 等
//  记录 list 调用次数 + 入参，方便断言"业务校验是否影响了 SQL 参数"
// ---------------------------------------------------------------------------
class InMemoryProblemRepository : public IProblemRepository {
public:
    std::optional<Problem> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        ++find_calls;
        last_find_id = id;
        for (const auto& p : problems_) {
            if (p.id == id) return p;
        }
        return std::nullopt;
    }

    ProblemListResult list(const ProblemListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        ++list_calls;
        last_query = q;

        ProblemListResult out;
        out.page = q.page;
        out.page_size = q.page_size;

        // 过滤
        for (const auto& p : problems_) {
            if (!q.include_unpublished && !p.is_published) continue;
            if (q.difficulty.has_value() && p.difficulty != *q.difficulty) continue;
            if (!q.q.empty() && p.title.find(q.q) == std::string::npos) continue;
            out.items.push_back(item_of(p));
        }
        out.total = static_cast<std::int64_t>(out.items.size());

        // 排序
        std::sort(out.items.begin(), out.items.end(), [&](const auto& a, const auto& b) {
            switch (q.sort) {
                case ProblemListQuery::Sort::IdDesc:        return a.id > b.id;
                case ProblemListQuery::Sort::CreatedDesc:   return a.created_at > b.created_at;
                case ProblemListQuery::Sort::PassRateDesc:  return a.pass_rate() > b.pass_rate();
            }
            return false;
        });

        // 分页
        const int start = (q.page - 1) * q.page_size;
        const int end   = start + q.page_size;
        if (start < static_cast<int>(out.items.size())) {
            out.items.erase(out.items.begin(), out.items.begin() + start);
            if (end < static_cast<int>(out.items.size())) {
                out.items.erase(out.items.begin() + (end - start), out.items.end());
            }
        } else {
            out.items.clear();
        }
        return out;
    }

    // 其他方法本任务范围不测
    Problem create(const Problem&) override { throw std::runtime_error("not impl"); }
    void update(const Problem&) override { throw std::runtime_error("not impl"); }
    void soft_delete(std::int64_t) override { throw std::runtime_error("not impl"); }
    void set_published(std::int64_t, bool) override { throw std::runtime_error("not impl"); }
    std::pair<int,int> submission_stats(std::int64_t) override { return {0,0}; }

    // 工具
    void add(Problem p, std::vector<Tag> tags = {}) {
        std::lock_guard<std::mutex> lk(mu_);
        ++next_id_;
        p.id = next_id_;
        problems_.push_back(std::move(p));
        tag_of_[p.id] = std::move(tags);
    }

    ProblemListItem item_of(const Problem& p) const {
        ProblemListItem it;
        it.id           = p.id;
        it.title        = p.title;
        it.difficulty   = p.difficulty;
        it.is_published = p.is_published;
        it.created_by   = p.created_by;
        it.created_at   = p.created_at;
        // 简单造几条 stats 数据：每个 id 给一个不同的通过率
        //   id=1: 1/10 = 0.1
        //   id=2: 7/10 = 0.7
        //   id=3: 9/10 = 0.9
        //   id=4: 5/10 = 0.5
        //   id=5: 2/10 = 0.2
        //   ... (n: min(n*2-1, 10) / 10)
        it.total_submissions    = 10;
        it.accepted_submissions = std::min(p.id * 2 - 1, std::int64_t{10});
        if (it.accepted_submissions < 0) it.accepted_submissions = 0;
        auto it2 = tag_of_.find(p.id);
        if (it2 != tag_of_.end()) it.tags = it2->second;
        return it;
    }

    // 断言助手
    int  list_calls  = 0;
    int  find_calls  = 0;
    std::int64_t last_find_id = 0;
    ProblemListQuery last_query;
    std::vector<Problem> problems_;

private:
    mutable std::mutex mu_;
    std::int64_t next_id_ = 0;
    std::map<std::int64_t, std::vector<Tag>> tag_of_;
};

Problem mkP(const std::string& title, Difficulty d, bool pub) {
    Problem p;
    p.title = title;
    p.content_md = "x";
    p.difficulty = d;
    p.is_published = pub;
    p.created_by = 1;
    p.created_at = "2026-04-23T10:00:00Z";
    return p;
}

// ===========================================================================
//  parse_problems_list_query 单元测试
// ===========================================================================
TEST(ProblemListQueryParseTest, EmptyParamsGivesDefaults) {
    std::multimap<std::string, std::string> params;
    auto p = parse_problems_list_query(params);
    EXPECT_TRUE(p.error_message.empty());
    EXPECT_EQ(p.query.page, 1);
    EXPECT_EQ(p.query.page_size, 20);
    EXPECT_FALSE(p.query.difficulty.has_value());
    EXPECT_TRUE(p.query.tag_slugs.empty());
    EXPECT_TRUE(p.query.q.empty());
    EXPECT_EQ(p.query.sort, ProblemListQuery::Sort::IdDesc);
    EXPECT_FALSE(p.query.include_unpublished);
}

TEST(ProblemListQueryParseTest, PageAcceptsPositiveInteger) {
    std::multimap<std::string, std::string> params{{"page","5"}};
    auto p = parse_problems_list_query(params);
    EXPECT_TRUE(p.error_message.empty());
    EXPECT_EQ(p.query.page, 5);
}

TEST(ProblemListQueryParseTest, PageZeroIsBadRequest) {
    std::multimap<std::string, std::string> params{{"page","0"}};
    auto p = parse_problems_list_query(params);
    EXPECT_FALSE(p.error_message.empty());
    EXPECT_NE(p.error_message.find("page"), std::string::npos);
}

TEST(ProblemListQueryParseTest, PageNegativeIsBadRequest) {
    std::multimap<std::string, std::string> params{{"page","-1"}};
    auto p = parse_problems_list_query(params);
    EXPECT_FALSE(p.error_message.empty());
}

TEST(ProblemListQueryParseTest, PageNonIntegerIsBadRequest) {
    std::multimap<std::string, std::string> params{{"page","abc"}};
    auto p = parse_problems_list_query(params);
    EXPECT_FALSE(p.error_message.empty());
}

TEST(ProblemListQueryParseTest, SizeAcceptsWhitelistedValues) {
    for (int sz : {10, 20, 50}) {
        std::multimap<std::string, std::string> params{{"size", std::to_string(sz)}};
        auto p = parse_problems_list_query(params);
        EXPECT_EQ(p.query.page_size, sz) << "size=" << sz;
    }
}

TEST(ProblemListQueryParseTest, SizeInvalidFallsBackToDefault) {
    for (int sz : {0, 5, 100, 999, -1}) {
        std::multimap<std::string, std::string> params{{"size", std::to_string(sz)}};
        auto p = parse_problems_list_query(params);
        EXPECT_EQ(p.query.page_size, 20) << "size=" << sz;
    }
}

TEST(ProblemListQueryParseTest, SizeNonIntegerFallsBackToDefault) {
    std::multimap<std::string, std::string> params{{"size","abc"}};
    auto p = parse_problems_list_query(params);
    EXPECT_EQ(p.query.page_size, 20);
}

TEST(ProblemListQueryParseTest, DifficultyAcceptsValid) {
    for (auto d : {"easy", "medium", "hard"}) {
        std::multimap<std::string, std::string> params{{"difficulty", d}};
        auto p = parse_problems_list_query(params);
        ASSERT_TRUE(p.query.difficulty.has_value());
    }
}

TEST(ProblemListQueryParseTest, DifficultyInvalidIsIgnored) {
    std::multimap<std::string, std::string> params{{"difficulty","EASY"},{"difficulty","impossible"}};
    auto p = parse_problems_list_query(params);
    EXPECT_FALSE(p.query.difficulty.has_value());
}

TEST(ProblemListQueryParseTest, SortAcceptsAllThree) {
    {
        std::multimap<std::string, std::string> params{{"sort","id_desc"}};
        EXPECT_EQ(parse_problems_list_query(params).query.sort, ProblemListQuery::Sort::IdDesc);
    }
    {
        std::multimap<std::string, std::string> params{{"sort","created_desc"}};
        EXPECT_EQ(parse_problems_list_query(params).query.sort, ProblemListQuery::Sort::CreatedDesc);
    }
    {
        std::multimap<std::string, std::string> params{{"sort","pass_rate_desc"}};
        EXPECT_EQ(parse_problems_list_query(params).query.sort, ProblemListQuery::Sort::PassRateDesc);
    }
}

TEST(ProblemListQueryParseTest, SortInvalidFallsBackToIdDesc) {
    std::multimap<std::string, std::string> params{{"sort","random_value"}};
    EXPECT_EQ(parse_problems_list_query(params).query.sort, ProblemListQuery::Sort::IdDesc);
}

TEST(ProblemListQueryParseTest, TagAcceptsSingleOccurrence) {
    std::multimap<std::string, std::string> params{{"tag","dp"}};
    auto p = parse_problems_list_query(params);
    ASSERT_EQ(p.query.tag_slugs.size(), 1u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
}

TEST(ProblemListQueryParseTest, TagAcceptsMultipleOccurrences) {
    std::multimap<std::string, std::string> params{{"tag","dp"},{"tag","array"}};
    auto p = parse_problems_list_query(params);
    ASSERT_EQ(p.query.tag_slugs.size(), 2u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
    EXPECT_EQ(p.query.tag_slugs[1], "array");
}

TEST(ProblemListQueryParseTest, TagAcceptsCommaSeparated) {
    std::multimap<std::string, std::string> params{{"tag","dp,array,graph"}};
    auto p = parse_problems_list_query(params);
    ASSERT_EQ(p.query.tag_slugs.size(), 3u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
    EXPECT_EQ(p.query.tag_slugs[1], "array");
    EXPECT_EQ(p.query.tag_slugs[2], "graph");
}

TEST(ProblemListQueryParseTest, TagAcceptsMixedMultiAndComma) {
    std::multimap<std::string, std::string> params{{"tag","dp,array"},{"tag","graph"},{"tag","tree"}};
    auto p = parse_problems_list_query(params);
    // 顺序：先来先得；相同 tag 去重
    ASSERT_EQ(p.query.tag_slugs.size(), 4u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
    EXPECT_EQ(p.query.tag_slugs[1], "array");
    EXPECT_EQ(p.query.tag_slugs[2], "graph");
    EXPECT_EQ(p.query.tag_slugs[3], "tree");
}

TEST(ProblemListQueryParseTest, TagDedupesDuplicates) {
    std::multimap<std::string, std::string> params{{"tag","dp"},{"tag","dp"},{"tag","dp"}};
    auto p = parse_problems_list_query(params);
    EXPECT_EQ(p.query.tag_slugs.size(), 1u);
}

TEST(ProblemListQueryParseTest, TagTrimsWhitespace) {
    std::multimap<std::string, std::string> params{{"tag"," dp , array "}};
    auto p = parse_problems_list_query(params);
    ASSERT_EQ(p.query.tag_slugs.size(), 2u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
    EXPECT_EQ(p.query.tag_slugs[1], "array");
}

TEST(ProblemListQueryParseTest, EmptyTagStringsAreSkipped) {
    std::multimap<std::string, std::string> params{{"tag",""},{"tag","dp"},{"tag",",,"}};
    auto p = parse_problems_list_query(params);
    ASSERT_EQ(p.query.tag_slugs.size(), 1u);
    EXPECT_EQ(p.query.tag_slugs[0], "dp");
}

TEST(ProblemListQueryParseTest, QFieldIsPreservedAsIs) {
    std::multimap<std::string, std::string> params{{"q","two sum"}};
    auto p = parse_problems_list_query(params);
    EXPECT_EQ(p.query.q, "two sum");
}

TEST(ProblemListQueryParseTest, PublicApiForcesIncludeUnpublishedFalse) {
    std::multimap<std::string, std::string> params{{"include_unpublished","1"}};
    auto p = parse_problems_list_query(params, /*is_admin=*/false);
    EXPECT_FALSE(p.query.include_unpublished);
}

TEST(ProblemListQueryParseTest, PublicApiIgnoresIncludeUnpublishedZero) {
    std::multimap<std::string, std::string> params{{"include_unpublished","0"}};
    auto p = parse_problems_list_query(params, /*is_admin=*/false);
    EXPECT_FALSE(p.query.include_unpublished);
}

TEST(ProblemListQueryParseTest, AdminApiRespectsIncludeUnpublishedTrue) {
    std::multimap<std::string, std::string> params{{"include_unpublished","1"}};
    auto p = parse_problems_list_query(params, /*is_admin=*/true);
    EXPECT_TRUE(p.query.include_unpublished);
}

TEST(ProblemListQueryParseTest, AdminApiRespectsIncludeUnpublishedFalse) {
    std::multimap<std::string, std::string> params{{"include_unpublished","0"}};
    auto p = parse_problems_list_query(params, /*is_admin=*/true);
    EXPECT_FALSE(p.query.include_unpublished);
}

TEST(ProblemListQueryParseTest, AdminApiDefaultsIncludeUnpublishedFalse) {
    std::multimap<std::string, std::string> params{};
    auto p = parse_problems_list_query(params, /*is_admin=*/true);
    EXPECT_FALSE(p.query.include_unpublished);
}

// ===========================================================================
//  ProblemService 行为测试（用 InMemoryProblemRepository）
// ===========================================================================
TEST(ProblemServiceTest, DelegatesListToRepo) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    repo->add(mkP("p1", Difficulty::Easy,   true));
    repo->add(mkP("p2", Difficulty::Medium, true));

    ProblemListQuery q;
    q.page = 1;
    auto r = svc->list(q);
    EXPECT_EQ(repo->list_calls, 1);
    EXPECT_EQ(r.total, 2);
    EXPECT_EQ(r.items.size(), 2u);
}

TEST(ProblemServiceTest, ListSanitizesPageSizeToWhitelist) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    for (int i = 0; i < 30; ++i) {
        repo->add(mkP("p" + std::to_string(i), Difficulty::Easy, true));
    }
    ProblemListQuery q;
    q.page = 1;
    q.page_size = 999;  // 非法
    auto r = svc->list(q);
    EXPECT_EQ(r.page_size, 20);  // 回到默认
    EXPECT_EQ(r.items.size(), 20u);
    EXPECT_EQ(r.total, 30);
}

TEST(ProblemServiceTest, ListSanitizesPageLessThan1) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    repo->add(mkP("p1", Difficulty::Easy, true));
    ProblemListQuery q;
    q.page = 0;
    auto r = svc->list(q);
    EXPECT_EQ(r.page, 1);
}

TEST(ProblemServiceTest, ListAcceptsSize10And50) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    for (int i = 0; i < 100; ++i) {
        repo->add(mkP("p" + std::to_string(i), Difficulty::Easy, true));
    }
    for (int sz : {10, 20, 50}) {
        ProblemListQuery q;
        q.page = 1;
        q.page_size = sz;
        auto r = svc->list(q);
        EXPECT_EQ(r.page_size, sz);
        EXPECT_EQ(r.items.size(), static_cast<std::size_t>(sz));
    }
}

TEST(ProblemServiceTest, ListFiltersByDifficulty) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    repo->add(mkP("e1", Difficulty::Easy,   true));
    repo->add(mkP("e2", Difficulty::Easy,   true));
    repo->add(mkP("h1", Difficulty::Hard,   true));
    ProblemListQuery q;
    q.page = 1;
    q.difficulty = Difficulty::Hard;
    auto r = svc->list(q);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].title, "h1");
}

TEST(ProblemServiceTest, ListExcludesUnpublishedByDefault) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    repo->add(mkP("pub",  Difficulty::Easy, true));
    repo->add(mkP("priv", Difficulty::Easy, false));
    auto r = svc->list({});
    EXPECT_EQ(r.total, 1);
    EXPECT_EQ(r.items[0].title, "pub");
}

TEST(ProblemServiceTest, ListSortsByPassRateDesc) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo);
    // 按 id 顺序插入：
    //   a (id=1): 1/10 = 0.1
    //   b (id=2): 3/10 = 0.3
    //   c (id=3): 5/10 = 0.5
    //   d (id=4): 7/10 = 0.7
    //   e (id=5): 9/10 = 0.9
    repo->add(mkP("a", Difficulty::Easy, true));
    repo->add(mkP("b", Difficulty::Easy, true));
    repo->add(mkP("c", Difficulty::Easy, true));
    repo->add(mkP("d", Difficulty::Easy, true));
    repo->add(mkP("e", Difficulty::Easy, true));
    ProblemListQuery q;
    q.sort = ProblemListQuery::Sort::PassRateDesc;
    auto r = svc->list(q);
    ASSERT_EQ(r.items.size(), 5u);
    EXPECT_EQ(r.items[0].title, "e");  // 0.9
    EXPECT_EQ(r.items[1].title, "d");  // 0.7
    EXPECT_EQ(r.items[2].title, "c");  // 0.5
    EXPECT_EQ(r.items[3].title, "b");  // 0.3
    EXPECT_EQ(r.items[4].title, "a");  // 0.1
}

}  // namespace
