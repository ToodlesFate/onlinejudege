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

#include <algorithm>
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
#include "domain/tag_repository.hpp"
#include "domain/testcase_repository.hpp"

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

// 极简 mock —— get_detail 路径才会用到；list 路径不访问
class MinimalTestcaseRepo : public oj::domain::ITestcaseRepository {
public:
    std::vector<oj::domain::Testcase> list_by_problem(std::int64_t) override { return {}; }
    std::vector<oj::domain::Testcase> list_samples(std::int64_t) override { return {}; }
    void create_many(std::int64_t, const std::vector<oj::domain::Testcase>&) override {}
    void replace_by_problem(std::int64_t, const std::vector<oj::domain::Testcase>&) override {}
    void delete_by_problem(std::int64_t) override {}
};
class MinimalTagRepo : public oj::domain::ITagRepository {
public:
    std::vector<oj::domain::Tag> list_all() override { return {}; }
    std::optional<oj::domain::Tag> find_by_id(int) override { return std::nullopt; }
    std::optional<oj::domain::Tag> find_by_slug(const std::string&) override { return std::nullopt; }
    std::vector<oj::domain::Tag> find_by_ids(const std::vector<int>&) override { return {}; }
    std::vector<oj::domain::Tag> tags_of_problem(std::int64_t) override { return {}; }
    std::vector<int> tag_ids_of_problem(std::int64_t) override { return {}; }
    void set_problem_tags(std::int64_t, const std::vector<int>&) override {}
};

// 富 mock —— get_detail 路径需要 list_samples + tags_of_problem 真实行为
class InMemoryTestcaseRepo : public oj::domain::ITestcaseRepository {
public:
    std::vector<oj::domain::Testcase> list_by_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<oj::domain::Testcase> out;
        for (const auto& c : cases_) {
            if (c.problem_id == pid) out.push_back(c);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
    std::vector<oj::domain::Testcase> list_samples(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<oj::domain::Testcase> out;
        for (const auto& c : cases_) {
            if (c.problem_id == pid && c.is_sample) out.push_back(c);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
    void create_many(std::int64_t pid, const std::vector<oj::domain::Testcase>& v) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& c : v) {
            oj::domain::Testcase copy = c;
            copy.problem_id = pid;
            cases_.push_back(copy);
        }
    }
    void replace_by_problem(std::int64_t pid, const std::vector<oj::domain::Testcase>& v) override {
        std::lock_guard<std::mutex> lk(mu_);
        cases_.erase(std::remove_if(cases_.begin(), cases_.end(),
                                     [pid](const oj::domain::Testcase& c) { return c.problem_id == pid; }),
                     cases_.end());
        for (auto& c : v) {
            oj::domain::Testcase copy = c;
            copy.problem_id = pid;
            cases_.push_back(copy);
        }
    }
    void delete_by_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        cases_.erase(std::remove_if(cases_.begin(), cases_.end(),
                                     [pid](const oj::domain::Testcase& c) { return c.problem_id == pid; }),
                     cases_.end());
    }
    void seed(std::int64_t pid, oj::domain::Testcase c) {
        std::lock_guard<std::mutex> lk(mu_);
        c.problem_id = pid;
        cases_.push_back(c);
    }
private:
    mutable std::mutex mu_;
    std::vector<oj::domain::Testcase> cases_;
};
class InMemoryTagRepo : public oj::domain::ITagRepository {
public:
    std::vector<oj::domain::Tag> list_all() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<oj::domain::Tag> out;
        for (const auto& [_, t] : tags_) out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        return out;
    }
    std::optional<oj::domain::Tag> find_by_id(int id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tags_.find(id);
        return it == tags_.end() ? std::nullopt
                                   : std::optional<oj::domain::Tag>(it->second);
    }
    std::optional<oj::domain::Tag> find_by_slug(const std::string& slug) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [_, t] : tags_) if (t.slug == slug) return t;
        return std::nullopt;
    }
    std::vector<oj::domain::Tag> find_by_ids(const std::vector<int>& ids) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<oj::domain::Tag> out;
        for (int id : ids) {
            auto it = tags_.find(id);
            if (it != tags_.end()) out.push_back(it->second);
        }
        return out;
    }
    std::vector<oj::domain::Tag> tags_of_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<oj::domain::Tag> out;
        auto range = prob_tag_.equal_range(pid);
        for (auto it = range.first; it != range.second; ++it) {
            auto t = tags_.find(it->second);
            if (t != tags_.end()) out.push_back(t->second);
        }
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        return out;
    }
    std::vector<int> tag_ids_of_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<int> out;
        auto range = prob_tag_.equal_range(pid);
        for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
        std::sort(out.begin(), out.end());
        return out;
    }
    void set_problem_tags(std::int64_t pid, const std::vector<int>& tag_ids) override {
        std::lock_guard<std::mutex> lk(mu_);
        prob_tag_.erase(pid);
        for (int id : tag_ids) prob_tag_.emplace(pid, id);
    }
    void seed_tag(oj::domain::Tag t) {
        std::lock_guard<std::mutex> lk(mu_);
        tags_[t.id] = t;
    }
private:
    mutable std::mutex mu_;
    std::map<int, oj::domain::Tag>             tags_;
    std::multimap<std::int64_t, int>           prob_tag_;  // pid → tag_id
};
TEST(ProblemServiceTest, DelegatesListToRepo) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
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
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
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
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
    repo->add(mkP("p1", Difficulty::Easy, true));
    ProblemListQuery q;
    q.page = 0;
    auto r = svc->list(q);
    EXPECT_EQ(r.page, 1);
}

TEST(ProblemServiceTest, ListAcceptsSize10And50) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
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
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
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
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
    repo->add(mkP("pub",  Difficulty::Easy, true));
    repo->add(mkP("priv", Difficulty::Easy, false));
    auto r = svc->list({});
    EXPECT_EQ(r.total, 1);
    EXPECT_EQ(r.items[0].title, "pub");
}

TEST(ProblemServiceTest, ListSortsByPassRateDesc) {
    auto repo = std::make_shared<InMemoryProblemRepository>();
    auto svc  = std::make_shared<ProblemService>(repo, std::make_shared<MinimalTestcaseRepo>(), std::make_shared<MinimalTagRepo>());
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

// ===========================================================================
//  get_detail —— ProblemService 行为测试
// ===========================================================================
TEST(ProblemServiceGetDetailTest, ReturnsNulloptForMissingId) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    auto d = svc->get_detail(999, /*include_unpublished=*/false);
    EXPECT_FALSE(d.has_value());
}

TEST(ProblemServiceGetDetailTest, ReturnsNulloptForUnpublishedWhenGateOff) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    repo->add(mkP("draft", Difficulty::Easy, /*pub=*/false));
    auto d = svc->get_detail(1, /*include_unpublished=*/false);
    EXPECT_FALSE(d.has_value());
}

TEST(ProblemServiceGetDetailTest, ReturnsDraftWhenAdminGateOn) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    repo->add(mkP("draft", Difficulty::Easy, /*pub=*/false));
    auto d = svc->get_detail(1, /*include_unpublished=*/true);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->problem.id, 1);
    EXPECT_FALSE(d->problem.is_published);
}

TEST(ProblemServiceGetDetailTest, ReturnsFullDetailForPublished) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    repo->add(mkP("两数之和", Difficulty::Easy, /*pub=*/true));

    // 灌 2 个 tag
    oj::domain::Tag arr{1, "数组", "数组"};
    oj::domain::Tag dp{7, "动态规划", "dp"};
    tags->seed_tag(arr);
    tags->seed_tag(dp);
    tags->set_problem_tags(1, {1, 7});

    // 灌 4 个 testcase：3 个 sample + 1 个 hidden
    oj::domain::Testcase c1; c1.case_index = 1; c1.input = "1 2"; c1.expected_output = "3";
    c1.is_sample = true; c1.score = 25;
    oj::domain::Testcase c2; c2.case_index = 2; c2.input = "10 20"; c2.expected_output = "30";
    c2.is_sample = true; c2.score = 25;
    oj::domain::Testcase c3; c3.case_index = 3; c3.input = "100 200"; c3.expected_output = "300";
    c3.is_sample = true; c3.score = 25;
    oj::domain::Testcase c4; c4.case_index = 4; c4.input = "big"; c4.expected_output = "big";
    c4.is_sample = false; c4.score = 25;
    cases->seed(1, c1);
    cases->seed(1, c2);
    cases->seed(1, c3);
    cases->seed(1, c4);

    auto d = svc->get_detail(1, /*include_unpublished=*/false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->problem.id, 1);
    EXPECT_EQ(d->problem.title, "两数之和");
    EXPECT_EQ(d->problem.difficulty, Difficulty::Easy);
    EXPECT_TRUE(d->problem.is_published);
    EXPECT_EQ(d->problem.time_limit_ms, 2000);
    EXPECT_EQ(d->problem.memory_limit_mb, 256);
    // tags：按 id ASC
    ASSERT_EQ(d->tags.size(), 2u);
    EXPECT_EQ(d->tags[0].id, 1);
    EXPECT_EQ(d->tags[0].name, "数组");
    EXPECT_EQ(d->tags[1].id, 7);
    EXPECT_EQ(d->tags[1].name, "动态规划");
    // sample_testcases：只 3 个，hidden 被过滤
    ASSERT_EQ(d->sample_testcases.size(), 3u);
    EXPECT_EQ(d->sample_testcases[0].case_index, 1);
    EXPECT_EQ(d->sample_testcases[0].input, "1 2");
    EXPECT_EQ(d->sample_testcases[0].expected_output, "3");
    EXPECT_TRUE(d->sample_testcases[0].is_sample);
    EXPECT_EQ(d->sample_testcases[2].case_index, 3);
}

TEST(ProblemServiceGetDetailTest, ReturnsEmptyTagsAndSamplesForProblemWithoutThem) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    repo->add(mkP("plain", Difficulty::Easy, true));
    auto d = svc->get_detail(1, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->tags.empty());
    EXPECT_TRUE(d->sample_testcases.empty());
}

TEST(ProblemServiceGetDetailTest, NoTestCasesReturnsEmptySamples) {
    auto repo   = std::make_shared<InMemoryProblemRepository>();
    auto cases  = std::make_shared<InMemoryTestcaseRepo>();
    auto tags   = std::make_shared<InMemoryTagRepo>();
    auto svc    = std::make_shared<ProblemService>(repo, cases, tags);
    repo->add(mkP("p", Difficulty::Easy, true));
    oj::domain::Testcase only_hidden;
    only_hidden.case_index = 1;
    only_hidden.input = "x"; only_hidden.expected_output = "y";
    only_hidden.is_sample = false; only_hidden.score = 100;
    cases->seed(1, only_hidden);
    auto d = svc->get_detail(1, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_TRUE(d->sample_testcases.empty())
        << "hidden test cases must not be returned to public";
}

// ===========================================================================
//  list_tags —— ProblemService 行为测试（SPEC §5.2.2 GET /api/tags）
// ===========================================================================
TEST(ProblemServiceListTagsTest, ReturnsEmptyListWhenRepoEmpty) {
    auto repo  = std::make_shared<InMemoryProblemRepository>();
    auto cases = std::make_shared<InMemoryTestcaseRepo>();
    auto tags  = std::make_shared<InMemoryTagRepo>();
    auto svc   = std::make_shared<ProblemService>(repo, cases, tags);
    auto out = svc->list_tags();
    EXPECT_TRUE(out.empty());
}

TEST(ProblemServiceListTagsTest, ReturnsAllSeededTags) {
    auto repo  = std::make_shared<InMemoryProblemRepository>();
    auto cases = std::make_shared<InMemoryTestcaseRepo>();
    auto tags  = std::make_shared<InMemoryTagRepo>();
    auto svc   = std::make_shared<ProblemService>(repo, cases, tags);
    // 灌 8 个 tag（SPEC §4.2 预置数据）
    oj::domain::Tag arr{1, "数组", "数组"};
    oj::domain::Tag str{2, "字符串", "string"};
    oj::domain::Tag ll{3, "链表", "linked-list"};
    oj::domain::Tag sq{4, "栈/队列", "stack-queue"};
    oj::domain::Tag tr{5, "树", "tree"};
    oj::domain::Tag gr{6, "图", "graph"};
    oj::domain::Tag dp{7, "动态规划", "dp"};
    oj::domain::Tag gr2{8, "贪心", "greedy"};
    for (const auto& t : {arr, str, ll, sq, tr, gr, dp, gr2}) tags->seed_tag(t);
    auto out = svc->list_tags();
    ASSERT_EQ(out.size(), 8u);
    // 按 id ASC
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(out[i].id, i + 1);
    }
    EXPECT_EQ(out[0].name, "数组");
    EXPECT_EQ(out[0].slug, "数组");
    EXPECT_EQ(out[1].name, "字符串");
    EXPECT_EQ(out[1].slug, "string");
    EXPECT_EQ(out[6].name, "动态规划");
    EXPECT_EQ(out[7].name, "贪心");
}

TEST(ProblemServiceListTagsTest, IsOrderStableAcrossCalls) {
    auto repo  = std::make_shared<InMemoryProblemRepository>();
    auto cases = std::make_shared<InMemoryTestcaseRepo>();
    auto tags  = std::make_shared<InMemoryTagRepo>();
    auto svc   = std::make_shared<ProblemService>(repo, cases, tags);
    oj::domain::Tag t1{1, "a", "a"};
    oj::domain::Tag t3{3, "c", "c"};
    oj::domain::Tag t2{2, "b", "b"};
    tags->seed_tag(t1);
    tags->seed_tag(t3);
    tags->seed_tag(t2);
    auto out1 = svc->list_tags();
    auto out2 = svc->list_tags();
    ASSERT_EQ(out1.size(), 3u);
    EXPECT_EQ(out1[0].id, 1);
    EXPECT_EQ(out1[1].id, 2);
    EXPECT_EQ(out1[2].id, 3);
    // 第二次调用顺序应一致
    EXPECT_EQ(out2[0].id, out1[0].id);
    EXPECT_EQ(out2[1].id, out1[1].id);
    EXPECT_EQ(out2[2].id, out1[2].id);
}

}  // namespace
