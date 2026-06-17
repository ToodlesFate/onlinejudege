// =============================================================================
//  test_problem_service_admin.cpp —— ProblemService 后台管理单元测试
//  不依赖 MySQL / HTTP；直接测 IProblemService 的 admin 5 个方法
//  + 业务校验 (build_problem / validate_cases / validate_tag_ids) 边界
//
//  覆盖：
//    1) create_problem / update_problem / delete_problem / set_published
//       / get_admin_detail 的 happy path
//    2) 业务校验失败全部翻译为 AdminProblemError(BadRequest)
//    3) 不存在 → AdminProblemError(NotFound)
//    4) DB 错误 → AdminProblemError(Internal)
//    5) 关键约束：tag / cases 必须真的写到对应 repo
//    6) update 保留 created_by / created_at
//    7) 失败回滚：tag / cases 写失败时把已建的 problem 也清掉
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"
#include "domain/tag_repository.hpp"
#include "domain/testcase_repository.hpp"

namespace {

using oj::domain::AdminProblemError;
using oj::domain::AdminProblemErrorKind;
using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::ITagRepository;
using oj::domain::ITestcaseRepository;
using oj::domain::Problem;
using oj::domain::ProblemListQuery;
using oj::domain::ProblemListResult;
using oj::domain::ProblemService;
using oj::domain::ProblemWriteInput;
using oj::domain::Tag;
using oj::domain::Testcase;

// ---------------------------------------------------------------------------
//  In-memory ProblemRepo —— 支持 create/update/soft_delete/set_published
//  + 可控"失败注入"（用 atomic 计数器模拟某次操作抛 runtime_error）
// ---------------------------------------------------------------------------
class InMemoryProblemRepo : public IProblemRepository {
public:
    std::optional<Problem> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& p : rows_) if (p.id == id) return p;
        return std::nullopt;
    }
    ProblemListResult list(const oj::domain::ProblemListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        oj::domain::ProblemListResult out;
        out.page = q.page; out.page_size = q.page_size;
        for (const auto& p : rows_) {
            if (!q.include_unpublished && !p.is_published) continue;
            oj::domain::ProblemListItem it;
            it.id = p.id; it.title = p.title; it.difficulty = p.difficulty;
            it.is_published = p.is_published; it.created_by = p.created_by;
            it.created_at = p.created_at;
            out.items.push_back(std::move(it));
        }
        out.total = static_cast<std::int64_t>(out.items.size());
        return out;
    }
    Problem create(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_create_.load()) {
            fail_create_.store(false);
            throw std::runtime_error("synthetic create failure");
        }
        Problem copy = p;
        copy.id = ++next_id_;
        copy.created_at = "2026-06-17T00:00:00Z";
        rows_.push_back(copy);
        ++create_count;
        return copy;
    }
    void update(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_update_.load()) {
            fail_update_.store(false);
            throw std::runtime_error("synthetic update failure");
        }
        for (auto& x : rows_) {
            if (x.id == p.id) {
                x.title           = p.title;
                x.content_md      = p.content_md;
                x.difficulty      = p.difficulty;
                x.time_limit_ms   = p.time_limit_ms;
                x.memory_limit_mb = p.memory_limit_mb;
                x.output_limit_mb = p.output_limit_mb;
                x.is_published    = p.is_published;
                ++update_count;
                return;
            }
        }
        throw std::runtime_error("problem not found: " + std::to_string(p.id));
    }
    void soft_delete(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& x : rows_) {
            if (x.id == id) {
                x.is_published = false;
                ++soft_delete_count;
                return;
            }
        }
        throw std::runtime_error("problem not found: " + std::to_string(id));
    }
    void set_published(std::int64_t id, bool pub) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& x : rows_) {
            if (x.id == id) {
                x.is_published = pub;
                ++set_published_count;
                return;
            }
        }
        throw std::runtime_error("problem not found: " + std::to_string(id));
    }
    std::pair<int, int> submission_stats(std::int64_t) override { return {0, 0}; }

    // 测试钩
    void fail_next_create()  { fail_create_.store(true); }
    void fail_next_update()  { fail_update_.store(true); }
    std::vector<Problem> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return rows_;
    }
    int  create_count        = 0;
    int  update_count        = 0;
    int  soft_delete_count   = 0;
    int  set_published_count = 0;

private:
    std::mutex            mu_;
    std::vector<Problem>  rows_;
    std::int64_t          next_id_{0};
    std::atomic<bool>     fail_create_{false};
    std::atomic<bool>     fail_update_{false};
};

// ---------------------------------------------------------------------------
//  In-memory TestcaseRepo —— 支持 list_by_problem / create_many / replace
//  + 失败注入
// ---------------------------------------------------------------------------
class InMemoryTestcaseRepo : public ITestcaseRepository {
public:
    std::vector<Testcase> list_by_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Testcase> out;
        for (const auto& t : rows_) if (t.problem_id == pid) out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
    std::vector<Testcase> list_samples(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Testcase> out;
        for (const auto& t : rows_) if (t.problem_id == pid && t.is_sample) out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
    void create_many(std::int64_t pid, const std::vector<Testcase>& cases) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_create_many_.load()) {
            fail_create_many_.store(false);
            throw std::runtime_error("synthetic create_many failure");
        }
        for (auto c : cases) {
            c.problem_id = pid;
            rows_.push_back(c);
        }
        ++create_many_count;
    }
    void replace_by_problem(std::int64_t pid, const std::vector<Testcase>& cases) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_replace_.load()) {
            fail_replace_.store(false);
            throw std::runtime_error("synthetic replace failure");
        }
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                                    [pid](const Testcase& t) { return t.problem_id == pid; }),
                    rows_.end());
        for (auto c : cases) { c.problem_id = pid; rows_.push_back(c); }
        ++replace_count;
    }
    void delete_by_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                                    [pid](const Testcase& t) { return t.problem_id == pid; }),
                    rows_.end());
    }
    void fail_next_create_many() { fail_create_many_.store(true); }
    void fail_next_replace()     { fail_replace_.store(true); }
    int create_many_count = 0;
    int replace_count     = 0;
    std::vector<Testcase> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return rows_;
    }

private:
    std::mutex              mu_;
    std::vector<Testcase>   rows_;
    std::atomic<bool>       fail_create_many_{false};
    std::atomic<bool>       fail_replace_{false};
};

// ---------------------------------------------------------------------------
//  In-memory TagRepo —— 支持 set_problem_tags + find_by_id + tags_of_problem
//  + 失败注入
// ---------------------------------------------------------------------------
class InMemoryTagRepo : public ITagRepository {
public:
    std::vector<Tag> list_all() override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Tag> out;
        for (const auto& t : tags_) out.push_back(t.second);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.id < b.id; });
        return out;
    }
    std::optional<Tag> find_by_id(int id) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = tags_.find(id);
        if (it == tags_.end()) return std::nullopt;
        return it->second;
    }
    std::optional<Tag> find_by_slug(const std::string& slug) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [_, t] : tags_) if (t.slug == slug) return t;
        return std::nullopt;
    }
    std::vector<Tag> find_by_ids(const std::vector<int>& ids) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Tag> out;
        for (int id : ids) {
            auto it = tags_.find(id);
            if (it != tags_.end()) out.push_back(it->second);
        }
        return out;
    }
    std::vector<Tag> tags_of_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Tag> out;
        auto range = pt_.equal_range(pid);
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
        auto range = pt_.equal_range(pid);
        for (auto it = range.first; it != range.second; ++it) out.push_back(it->second);
        std::sort(out.begin(), out.end());
        return out;
    }
    void set_problem_tags(std::int64_t pid, const std::vector<int>& tag_ids) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (fail_set_.load()) {
            fail_set_.store(false);
            throw std::runtime_error("synthetic set_problem_tags failure");
        }
        pt_.erase(pid);
        for (int id : tag_ids) pt_.emplace(pid, id);
        ++set_count;
    }
    void seed(Tag t) {
        std::lock_guard<std::mutex> lk(mu_);
        tags_[t.id] = t;
    }
    void fail_next_set() { fail_set_.store(true); }
    int set_count = 0;

private:
    std::mutex                       mu_;
    std::map<int, Tag>               tags_;
    std::multimap<std::int64_t, int> pt_;
    std::atomic<bool>                fail_set_{false};
};

// ---------------------------------------------------------------------------
//  Helper —— 默认合法输入
// ---------------------------------------------------------------------------
ProblemWriteInput make_input(bool is_published = false,
                             std::vector<int> tag_ids = {},
                             std::vector<Testcase> cases = {}) {
    ProblemWriteInput in;
    in.title            = "两数之和";
    in.content_md       = "读入 a, b；输出 a+b";
    in.difficulty_str   = "easy";
    in.time_limit_ms    = 2000;
    in.memory_limit_mb  = 256;
    in.output_limit_mb  = 64;
    in.is_published     = is_published;
    in.tag_ids          = std::move(tag_ids);
    if (cases.empty()) {
        Testcase c1; c1.case_index = 1; c1.input = "1 2"; c1.expected_output = "3";
        c1.is_sample = true; c1.score = 60;
        Testcase c2; c2.case_index = 2; c2.input = "10 20"; c2.expected_output = "30";
        c2.is_sample = true; c2.score = 40;
        cases = {c1, c2};
    }
    in.cases = std::move(cases);
    return in;
}

struct ServiceBundle {
    std::shared_ptr<InMemoryProblemRepo>   problems;
    std::shared_ptr<InMemoryTestcaseRepo>  testcases;
    std::shared_ptr<InMemoryTagRepo>       tags;
    std::shared_ptr<ProblemService>        service;
};

ServiceBundle make_service() {
    ServiceBundle b;
    b.problems  = std::make_shared<InMemoryProblemRepo>();
    b.testcases = std::make_shared<InMemoryTestcaseRepo>();
    b.tags      = std::make_shared<InMemoryTagRepo>();
    b.service   = std::make_shared<ProblemService>(b.problems, b.testcases, b.tags);
    return b;
}

// ===========================================================================
//  get_admin_detail
// ===========================================================================
TEST(ProblemServiceAdminGetDetailTest, ReturnsFullDetailWithAllTestcases) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->seed({7, "动态规划", "dp"});

    auto in = make_input(/*is_published=*/true, /*tag_ids=*/{1, 7});
    auto p = b.service->create_problem(/*created_by=*/1, in);
    ASSERT_EQ(p.id, 1);

    auto detail = b.service->get_admin_detail(1);
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->problem.id, 1);
    EXPECT_EQ(detail->problem.title, "两数之和");
    EXPECT_EQ(detail->problem.difficulty, Difficulty::Easy);
    EXPECT_EQ(detail->tags.size(), 2u);
    EXPECT_EQ(detail->tags[0].id, 1);
    EXPECT_EQ(detail->tags[1].id, 7);
    // 2 个 testcase
    ASSERT_EQ(detail->testcases.size(), 2u);
    EXPECT_EQ(detail->testcases[0].case_index, 1);
    EXPECT_EQ(detail->testcases[0].input, "1 2");
    EXPECT_EQ(detail->testcases[0].expected_output, "3");
    EXPECT_TRUE(detail->testcases[0].is_sample);
    EXPECT_EQ(detail->testcases[0].score, 60);
    EXPECT_EQ(detail->testcases[1].case_index, 2);
    EXPECT_EQ(detail->testcases[1].score, 40);
}

TEST(ProblemServiceAdminGetDetailTest, ReturnsAllTestcasesIncludingHidden) {
    ServiceBundle b = make_service();
    Testcase h1; h1.case_index = 1; h1.input = "a"; h1.expected_output = "1";
    h1.is_sample = false; h1.score = 50;
    Testcase h2; h2.case_index = 2; h2.input = "b"; h2.expected_output = "2";
    h2.is_sample = true;  h2.score = 30;
    Testcase h3; h3.case_index = 3; h3.input = "c"; h3.expected_output = "3";
    h3.is_sample = false; h3.score = 20;
    auto in = make_input(false, {}, {h1, h2, h3});
    b.service->create_problem(1, in);

    auto detail = b.service->get_admin_detail(1);
    ASSERT_TRUE(detail.has_value());
    EXPECT_EQ(detail->testcases.size(), 3u);
    int hidden = 0, sample = 0;
    for (const auto& c : detail->testcases) {
        if (c.is_sample) ++sample; else ++hidden;
    }
    EXPECT_EQ(hidden, 2);
    EXPECT_EQ(sample, 1);
}

TEST(ProblemServiceAdminGetDetailTest, NotFoundReturnsNullopt) {
    ServiceBundle b = make_service();
    EXPECT_FALSE(b.service->get_admin_detail(999).has_value());
}

TEST(ProblemServiceAdminGetDetailTest, ReturnsEmptyTagsAndCasesForNewProblem) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "x"; c.expected_output = "x";
    c.is_sample = true; c.score = 100;
    auto in = make_input(false, {}, {c});
    b.service->create_problem(1, in);
    auto detail = b.service->get_admin_detail(1);
    ASSERT_TRUE(detail.has_value());
    EXPECT_TRUE(detail->tags.empty());
    ASSERT_EQ(detail->testcases.size(), 1u);
}

// ===========================================================================
//  create_problem —— happy path
// ===========================================================================
TEST(ProblemServiceAdminCreateTest, HappyPathPersistsAllData) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->seed({7, "动态规划", "dp"});

    auto in = make_input(true, {1, 7});
    Problem p;
    try {
        p = b.service->create_problem(/*created_by=*/42, in);
    } catch (const AdminProblemError& e) {
        FAIL() << "unexpected error: " << e.what();
    }
    EXPECT_EQ(p.id, 1);
    EXPECT_EQ(p.title, "两数之和");
    EXPECT_EQ(p.difficulty, Difficulty::Easy);
    EXPECT_EQ(p.time_limit_ms, 2000);
    EXPECT_EQ(p.memory_limit_mb, 256);
    EXPECT_EQ(p.output_limit_mb, 64);
    EXPECT_TRUE(p.is_published);
    EXPECT_EQ(p.created_by, 42);
    EXPECT_FALSE(p.created_at.empty());

    // 验证三个 repo 都被正确写入
    EXPECT_EQ(b.problems->create_count, 1);
    EXPECT_EQ(b.testcases->create_many_count, 1);
    EXPECT_EQ(b.tags->set_count, 1);
    auto snap = b.problems->snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].created_by, 42);
    auto tc_snap = b.testcases->snapshot();
    ASSERT_EQ(tc_snap.size(), 2u);
    EXPECT_EQ(tc_snap[0].score + tc_snap[1].score, 100);
    auto tag_ids = b.tags->tag_ids_of_problem(1);
    EXPECT_EQ(tag_ids.size(), 2u);
}

TEST(ProblemServiceAdminCreateTest, IsPublishedFalseDraft) {
    ServiceBundle b = make_service();
    auto in = make_input(/*is_published=*/false);
    Problem p;
    try {
        p = b.service->create_problem(1, in);
    } catch (const AdminProblemError& e) {
        FAIL() << e.what();
    }
    EXPECT_FALSE(p.is_published);
}

TEST(ProblemServiceAdminCreateTest, NoTagsAllowed) {
    ServiceBundle b = make_service();
    auto in = make_input(false, /*tag_ids=*/{});
    EXPECT_NO_THROW(b.service->create_problem(1, in));
    EXPECT_EQ(b.tags->set_count, 0);
}

TEST(ProblemServiceAdminCreateTest, AllThreeDifficultiesAccepted) {
    for (auto d : {"easy", "medium", "hard"}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.difficulty_str = d;
        Problem p;
        try {
            p = b.service->create_problem(1, in);
        } catch (const AdminProblemError& e) {
            FAIL() << d << ": " << e.what();
        }
        EXPECT_FALSE(p.id == 0);
    }
}

// ===========================================================================
//  create_problem —— 业务校验（全部应抛 BadRequest）
// ===========================================================================
TEST(ProblemServiceAdminCreateTest, RejectsEmptyTitle) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.title = "";
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("title"), std::string::npos);
    }
    EXPECT_EQ(b.problems->create_count, 0);
}

TEST(ProblemServiceAdminCreateTest, TrimsTitleWhitespace) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.title = "   valid   ";
    Problem p;
    try {
        p = b.service->create_problem(1, in);
    } catch (const AdminProblemError& e) {
        FAIL() << e.what();
    }
    EXPECT_EQ(p.title, "valid");
}

TEST(ProblemServiceAdminCreateTest, RejectsTitleWithOnlyWhitespace) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.title = "   \t  ";
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, AcceptsTitleExactly100Chars) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.title = std::string(100, 'x');
    EXPECT_NO_THROW(b.service->create_problem(1, in));
}

TEST(ProblemServiceAdminCreateTest, RejectsTitleOver100Chars) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.title = std::string(101, 'x');
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("100"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsEmptyContentMd) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.content_md = "";
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, AcceptsContentMdExactly64KB) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.content_md = std::string(65536, 'x');
    EXPECT_NO_THROW(b.service->create_problem(1, in));
}

TEST(ProblemServiceAdminCreateTest, RejectsContentMdOver64KB) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.content_md = std::string(65537, 'x');
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsInvalidDifficulty) {
    ServiceBundle b = make_service();
    for (const char* d : {"", "EASY", "impossible", "easy ", "Easy"}) {
        auto in = make_input();
        in.difficulty_str = d;
        try {
            b.service->create_problem(1, in);
            FAIL() << "difficulty should be rejected: " << d;
        } catch (const AdminProblemError& e) {
            EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest) << d;
        }
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsOutOfRangeTimeLimit) {
    for (int v : {0, -1, 10001, 99999}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.time_limit_ms = v;
        try {
            b.service->create_problem(1, in);
            FAIL() << "time_limit_ms should be rejected: " << v;
        } catch (const AdminProblemError& e) {
            EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest) << v;
        }
    }
}

TEST(ProblemServiceAdminCreateTest, AcceptsTimeLimitBoundaries) {
    for (int v : {1, 10000}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.time_limit_ms = v;
        EXPECT_NO_THROW(b.service->create_problem(1, in)) << v;
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsOutOfRangeMemoryLimit) {
    for (int v : {0, 32, 63, 1025, 9999}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.memory_limit_mb = v;
        try {
            b.service->create_problem(1, in);
            FAIL() << "memory_limit_mb should be rejected: " << v;
        } catch (const AdminProblemError& e) {
            EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest) << v;
        }
    }
}

TEST(ProblemServiceAdminCreateTest, AcceptsMemoryLimitBoundaries) {
    for (int v : {64, 1024}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.memory_limit_mb = v;
        EXPECT_NO_THROW(b.service->create_problem(1, in)) << v;
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsOutOfRangeOutputLimit) {
    for (int v : {0, 257, 9999}) {
        ServiceBundle b = make_service();
        auto in = make_input();
        in.output_limit_mb = v;
        try {
            b.service->create_problem(1, in);
            FAIL() << "output_limit_mb should be rejected: " << v;
        } catch (const AdminProblemError& e) {
            EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest) << v;
        }
    }
}

// ===========================================================================
//  create_problem —— testcases 校验
// ===========================================================================
TEST(ProblemServiceAdminCreateTest, RejectsEmptyCases) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases.clear();
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("at least 1"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsOver100Cases) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases.clear();
    // 101 个 case：触发"超过 100"拒绝；score 全部 1（总和 101 也会被拒，但先被 count 卡住）
    for (int i = 1; i <= 101; ++i) {
        Testcase c; c.case_index = i; c.input = "x"; c.expected_output = "y";
        c.is_sample = false; c.score = 1;
        in.cases.push_back(c);
    }
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("100"), std::string::npos);
    }
    // 验证 problem 没被建出来（前置校验失败，不应写库）
    EXPECT_EQ(b.problems->create_count, 0);
}

TEST(ProblemServiceAdminCreateTest, AcceptsExactly100Cases) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases.clear();
    for (int i = 1; i <= 100; ++i) {
        Testcase c; c.case_index = i; c.input = "x"; c.expected_output = "y";
        c.is_sample = (i == 1); c.score = 1;
        in.cases.push_back(c);
    }
    EXPECT_NO_THROW(b.service->create_problem(1, in));
}

TEST(ProblemServiceAdminCreateTest, RejectsCaseIndexZero) {
    ServiceBundle b = make_service();
    auto in = make_input();
    Testcase c; c.case_index = 0; c.input = "x"; c.expected_output = "y";
    c.is_sample = true; c.score = 100;
    in.cases = {c};
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("case_index"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsCaseIndexOver100) {
    ServiceBundle b = make_service();
    auto in = make_input();
    Testcase c; c.case_index = 101; c.input = "x"; c.expected_output = "y";
    c.is_sample = true; c.score = 100;
    in.cases = {c};
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsDuplicateCaseIndex) {
    ServiceBundle b = make_service();
    auto in = make_input();
    Testcase c1; c1.case_index = 1; c1.input = "a"; c1.expected_output = "1";
    c1.is_sample = true; c1.score = 50;
    Testcase c2; c2.case_index = 1; c2.input = "b"; c2.expected_output = "2";
    c2.is_sample = true; c2.score = 50;
    in.cases = {c1, c2};
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("duplicate"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsNegativeScore) {
    ServiceBundle b = make_service();
    auto in = make_input();
    Testcase c1; c1.case_index = 1; c1.input = "a"; c1.expected_output = "1";
    c1.is_sample = true; c1.score = 50;
    Testcase c2; c2.case_index = 2; c2.input = "b"; c2.expected_output = "2";
    c2.is_sample = true; c2.score = -1;
    in.cases = {c1, c2};
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsScoreOver100) {
    ServiceBundle b = make_service();
    auto in = make_input();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 101;
    in.cases = {c};
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsScoreSumNot100_TooSmall) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases[0].score = 30; in.cases[1].score = 30;  // sum=60
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("100"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsScoreSumNot100_TooLarge) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases[0].score = 80; in.cases[1].score = 80;  // sum=160
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, AcceptsScoreZeroCase) {
    ServiceBundle b = make_service();
    auto in = make_input();
    in.cases[0].score = 100; in.cases[1].score = 0;  // sum=100，含 0 分点
    EXPECT_NO_THROW(b.service->create_problem(1, in));
}

// ===========================================================================
//  create_problem —— tag_ids 校验
// ===========================================================================
TEST(ProblemServiceAdminCreateTest, RejectsTagIdZero) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    auto in = make_input(false, {0});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsTagIdNegative) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    auto in = make_input(false, {-1});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsDuplicateTagId) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    auto in = make_input(false, {1, 1});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("duplicate"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, RejectsUnknownTagId) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    auto in = make_input(false, {999});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("999"), std::string::npos);
    }
}

TEST(ProblemServiceAdminCreateTest, MultipleValidTagsPersisted) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->seed({2, "字符串", "string"});
    b.tags->seed({3, "链表", "linked-list"});
    auto in = make_input(false, {1, 2, 3});
    EXPECT_NO_THROW(b.service->create_problem(1, in));
    auto ids = b.tags->tag_ids_of_problem(1);
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[2], 3);
}

// ===========================================================================
//  create_problem —— 失败回滚
// ===========================================================================
TEST(ProblemServiceAdminCreateTest, RollsBackProblemOnTagFailure) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->fail_next_set();
    auto in = make_input(false, {1});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
    // problem 行应该被回滚（soft_delete 触发）
    auto snap = b.problems->snapshot();
    // problem 行已建但 is_published=false（被回滚为草稿）—— 仍存在于 repo
    // 关键：不应有可用的 published problem
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_FALSE(snap[0].is_published);
    EXPECT_EQ(b.problems->soft_delete_count, 1);
}

TEST(ProblemServiceAdminCreateTest, RollsBackProblemOnTestcaseFailure) {
    ServiceBundle b = make_service();
    b.testcases->fail_next_create_many();
    auto in = make_input(false, {});
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
    auto snap = b.problems->snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_FALSE(snap[0].is_published);  // 回滚后 is_published=false
    EXPECT_EQ(b.problems->soft_delete_count, 1);
}

TEST(ProblemServiceAdminCreateTest, RepoCreateFailureReturnsInternal) {
    ServiceBundle b = make_service();
    b.problems->fail_next_create();
    auto in = make_input();
    try {
        b.service->create_problem(1, in);
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
    // 后续 repo 都没被调用
    EXPECT_EQ(b.testcases->create_many_count, 0);
    EXPECT_EQ(b.tags->set_count, 0);
}

// ===========================================================================
//  update_problem
// ===========================================================================
TEST(ProblemServiceAdminUpdateTest, HappyPathReplacesAllFields) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->seed({7, "动态规划", "dp"});
    // 先建
    auto in = make_input(false, {1});
    auto p = b.service->create_problem(1, in);
    ASSERT_EQ(p.id, 1);

    // 改：换 title、难度、limits、tags、cases
    auto upd = make_input(true, {1, 7});
    upd.title = "新标题";
    upd.difficulty_str = "hard";
    upd.time_limit_ms = 1500;
    upd.memory_limit_mb = 128;
    Testcase c1; c1.case_index = 1; c1.input = "x"; c1.expected_output = "y";
    c1.is_sample = true; c1.score = 100;
    upd.cases = {c1};
    EXPECT_NO_THROW(b.service->update_problem(1, upd));

    // 验证 DB
    EXPECT_EQ(b.problems->update_count, 1);
    EXPECT_EQ(b.testcases->replace_count, 1);
    EXPECT_EQ(b.tags->set_count, 2);  // 1 from create, 1 from update
    auto snap = b.problems->snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_EQ(snap[0].title, "新标题");
    EXPECT_EQ(snap[0].difficulty, Difficulty::Hard);
    EXPECT_EQ(snap[0].time_limit_ms, 1500);
    EXPECT_EQ(snap[0].memory_limit_mb, 128);
    EXPECT_TRUE(snap[0].is_published);
    // cases 全量替换
    auto tc = b.testcases->snapshot();
    EXPECT_EQ(tc.size(), 1u);
    // tags 更新到 {1, 7}
    auto tids = b.tags->tag_ids_of_problem(1);
    EXPECT_EQ(tids.size(), 2u);
}

TEST(ProblemServiceAdminUpdateTest, PreservesCreatedByAndCreatedAt) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    auto in = make_input(false, {}, {c});
    auto p = b.service->create_problem(/*created_by=*/99, in);
    const std::string orig_created_at = p.created_at;
    ASSERT_FALSE(orig_created_at.empty());

    auto upd = make_input(true, {}, {c});
    EXPECT_NO_THROW(b.service->update_problem(1, upd));
    auto snap = b.problems->snapshot();
    EXPECT_EQ(snap[0].created_by, 99);
    EXPECT_EQ(snap[0].created_at, orig_created_at);
}

TEST(ProblemServiceAdminUpdateTest, NotFoundReturnsNotFound) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    auto in = make_input(false, {}, {c});
    try {
        b.service->update_problem(999, in);
        FAIL() << "expected NotFound";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::NotFound);
        EXPECT_NE(std::string{e.what()}.find("999"), std::string::npos);
    }
    EXPECT_EQ(b.problems->update_count, 0);
}

TEST(ProblemServiceAdminUpdateTest, ValidationFailureSkipsAllRepoCalls) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));

    // 改个不合法版本
    auto bad = make_input(false, {}, {c});
    bad.title = std::string(200, 'x');  // > 100
    try {
        b.service->update_problem(1, bad);
        FAIL() << "expected BadRequest";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::BadRequest);
    }
    EXPECT_EQ(b.problems->update_count, 0);
    EXPECT_EQ(b.testcases->replace_count, 0);
}

TEST(ProblemServiceAdminUpdateTest, RepoUpdateFailureReturnsInternal) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));
    b.problems->fail_next_update();
    try {
        b.service->update_problem(1, make_input(false, {}, {c}));
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
}

TEST(ProblemServiceAdminUpdateTest, TagFailureReturnsInternal) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {1}, {c}));
    b.tags->fail_next_set();
    try {
        b.service->update_problem(1, make_input(false, {1}, {c}));
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
}

TEST(ProblemServiceAdminUpdateTest, TestcaseReplaceFailureReturnsInternal) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));
    b.testcases->fail_next_replace();
    try {
        b.service->update_problem(1, make_input(false, {}, {c}));
        FAIL() << "expected Internal";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::Internal);
    }
}

// ===========================================================================
//  delete_problem —— 软删
// ===========================================================================
TEST(ProblemServiceAdminDeleteTest, HappyPathCallsSoftDelete) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(true, {}, {c}));
    EXPECT_NO_THROW(b.service->delete_problem(1));
    EXPECT_EQ(b.problems->soft_delete_count, 1);
    auto snap = b.problems->snapshot();
    EXPECT_FALSE(snap[0].is_published);
}

TEST(ProblemServiceAdminDeleteTest, NotFoundReturnsNotFound) {
    ServiceBundle b = make_service();
    try {
        b.service->delete_problem(999);
        FAIL() << "expected NotFound";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::NotFound);
    }
}

TEST(ProblemServiceAdminDeleteTest, AlreadyUnpublishedIsIdempotent) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));
    EXPECT_NO_THROW(b.service->delete_problem(1));
    EXPECT_EQ(b.problems->soft_delete_count, 1);
}

// ===========================================================================
//  set_published
// ===========================================================================
TEST(ProblemServiceAdminSetPublishedTest, PublishTrue) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));
    EXPECT_NO_THROW(b.service->set_published(1, true));
    EXPECT_EQ(b.problems->set_published_count, 1);
    EXPECT_TRUE(b.problems->snapshot()[0].is_published);
}

TEST(ProblemServiceAdminSetPublishedTest, PublishFalse) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(true, {}, {c}));
    EXPECT_NO_THROW(b.service->set_published(1, false));
    EXPECT_EQ(b.problems->set_published_count, 1);
    EXPECT_FALSE(b.problems->snapshot()[0].is_published);
}

TEST(ProblemServiceAdminSetPublishedTest, NotFoundReturnsNotFound) {
    ServiceBundle b = make_service();
    try {
        b.service->set_published(999, true);
        FAIL() << "expected NotFound";
    } catch (const AdminProblemError& e) {
        EXPECT_EQ(e.kind(), AdminProblemErrorKind::NotFound);
    }
}

TEST(ProblemServiceAdminSetPublishedTest, IdempotentToggle) {
    ServiceBundle b = make_service();
    Testcase c; c.case_index = 1; c.input = "a"; c.expected_output = "1";
    c.is_sample = true; c.score = 100;
    b.service->create_problem(1, make_input(false, {}, {c}));
    b.service->set_published(1, true);
    b.service->set_published(1, false);
    b.service->set_published(1, true);
    EXPECT_EQ(b.problems->set_published_count, 3);
    EXPECT_TRUE(b.problems->snapshot()[0].is_published);
}

// ===========================================================================
//  AdminProblemError 异常类型
// ===========================================================================
TEST(AdminProblemErrorTest, KindAndMessagePreserved) {
    AdminProblemError e_b(AdminProblemErrorKind::BadRequest, "bad");
    AdminProblemError e_n(AdminProblemErrorKind::NotFound,   "nope");
    AdminProblemError e_i(AdminProblemErrorKind::Internal,   "boom");
    EXPECT_EQ(e_b.kind(), AdminProblemErrorKind::BadRequest);
    EXPECT_EQ(e_n.kind(), AdminProblemErrorKind::NotFound);
    EXPECT_EQ(e_i.kind(), AdminProblemErrorKind::Internal);
    EXPECT_STREQ(e_b.what(), "bad");
    EXPECT_STREQ(e_n.what(), "nope");
    EXPECT_STREQ(e_i.what(), "boom");
}

TEST(AdminProblemErrorTest, IsStdException) {
    AdminProblemError e(AdminProblemErrorKind::BadRequest, "x");
    try {
        throw e;
    } catch (const std::exception& base) {
        EXPECT_STREQ(base.what(), "x");
    }
}

// ===========================================================================
//  end-to-end：admin 完整生命周期
// ===========================================================================
TEST(ProblemServiceAdminE2ETest, CreateUpdateListDeleteCycle) {
    ServiceBundle b = make_service();
    b.tags->seed({1, "数组", "数组"});
    b.tags->seed({7, "动态规划", "dp"});

    // 1) CREATE
    auto in = make_input(false, {1, 7});
    Problem p;
    try {
        p = b.service->create_problem(/*created_by=*/1, in);
    } catch (const AdminProblemError& e) { FAIL() << e.what(); }
    EXPECT_EQ(p.id, 1);
    EXPECT_FALSE(p.is_published);

    // 2) LIST —— admin 视角应能看到（含草稿）
    auto listed = b.service->list({});  // include_unpublished=false —— 默认
    // 默认是不含草稿的；这里用 include_unpublished=true
    oj::domain::ProblemListQuery q; q.include_unpublished = true;
    listed = b.service->list(q);
    EXPECT_EQ(listed.total, 1);

    // 3) EDIT-DATA
    auto d = b.service->get_admin_detail(1);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->testcases.size(), 2u);
    EXPECT_EQ(d->tags.size(), 2u);

    // 4) UPDATE：换 title + 升 hard + 改 cases
    auto upd = make_input(true, {7});  // 移除数组 tag
    upd.title = "两数之和 v2";
    upd.difficulty_str = "hard";
    Testcase c; c.case_index = 1; c.input = "1 2"; c.expected_output = "3";
    c.is_sample = true; c.score = 100;
    upd.cases = {c};
    EXPECT_NO_THROW(b.service->update_problem(1, upd));

    d = b.service->get_admin_detail(1);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->problem.title, "两数之和 v2");
    EXPECT_EQ(d->problem.difficulty, Difficulty::Hard);
    EXPECT_EQ(d->testcases.size(), 1u);
    EXPECT_EQ(d->tags.size(), 1u);
    EXPECT_EQ(d->tags[0].id, 7);

    // 5) PATCH publish: 不变（仍 published=true）
    EXPECT_TRUE(d->problem.is_published);
    b.service->set_published(1, false);
    d = b.service->get_admin_detail(1);
    EXPECT_FALSE(d->problem.is_published);

    // 6) DELETE 软删
    EXPECT_NO_THROW(b.service->delete_problem(1));
    d = b.service->get_admin_detail(1);
    EXPECT_FALSE(d->problem.is_published);
    EXPECT_EQ(b.problems->soft_delete_count, 1);
}

}  // namespace
