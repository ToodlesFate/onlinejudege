// =============================================================================
//  test_submission_service_edges.cpp —— SubmissionService 边界 / 异常路径测试
//  补 test_submission_service.cpp 之外的细节场景：
//    - 错误信息文本（handler 不需要做内容断言，但日志 / 客户端诊断时要稳定）
//    - code_max_bytes() 透传
//    - get_detail 在 testcases_ 为 null 时的降级
//    - get_detail 在 list_samples 抛异常时的降级
//    - 题目存在但完全没有 testcase
//    - submission_cases 表为空 → cases 数组为空
//    - username 字段透传（不应该是空）
//    - 多提交：同一用户同一题目多次提交互不干扰
//    - 跨用户：user 1 和 user 2 提交同题可见性独立
//    - 题目存在但 is_published=true 边界（false 必须拒绝）
//    - 异常传播：repo::get_full 抛 → service 抛 GetSubmissionError(Internal)
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "domain/problem_repository.hpp"
#include "domain/problem_types.hpp"
#include "domain/submission_repository.hpp"
#include "domain/submission_service.hpp"
#include "domain/submission_types.hpp"
#include "domain/testcase_repository.hpp"

namespace {

using oj::domain::ClaimedTask;
using oj::domain::CreateSubmissionError;
using oj::domain::CreateSubmissionErrorKind;
using oj::domain::Difficulty;
using oj::domain::GetSubmissionError;
using oj::domain::GetSubmissionErrorKind;
using oj::domain::IProblemRepository;
using oj::domain::ISubmissionRepository;
using oj::domain::ITestcaseRepository;
using oj::domain::JudgeTaskPayload;
using oj::domain::Language;
using oj::domain::Problem;
using oj::domain::ProblemListItem;
using oj::domain::ProblemListQuery;
using oj::domain::ProblemListResult;
using oj::domain::Submission;
using oj::domain::SubmissionCase;
using oj::domain::SubmissionDetail;
using oj::domain::SubmissionListQuery;
using oj::domain::SubmissionListResult;
using oj::domain::SubmissionResult;
using oj::domain::SubmissionService;
using oj::domain::SubmissionStatus;
using oj::domain::Testcase;

// ---------------------------------------------------------------------------
//  In-memory repos（与 test_submission_service.cpp 一致的最小集）
// ---------------------------------------------------------------------------
class InMemorySubmissionRepo : public ISubmissionRepository {
public:
    std::int64_t create(std::int64_t user_id, std::int64_t problem_id,
                        Language language, std::string_view code) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto id = ++next_id_;
        Submission s;
        s.id         = id;
        s.user_id    = user_id;
        s.problem_id = problem_id;
        s.language   = language;
        s.code       = std::string{code};
        s.status     = SubmissionStatus::Queued;
        s.created_at = "2026-06-17T00:00:00Z";
        rows_.push_back(s);
        return id;
    }
    bool claim_one(ClaimedTask&) override { return false; }
    JudgeTaskPayload load_task(std::int64_t) override { return {}; }
    void update_status(std::int64_t, SubmissionStatus) override {}
    void finish(std::int64_t id, SubmissionResult r, int score, int, int,
                std::string_view, std::string_view) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) {
            if (s.id == id) {
                s.status = SubmissionStatus::Finished;
                s.result = r;
                s.total_score = score;
                s.finished_at = "2026-06-17T00:00:01Z";
            }
        }
    }
    void insert_case(std::int64_t sub_id, const SubmissionCase& c) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto sc = c;
        sc.submission_id = sub_id;
        cases_.push_back(sc);
    }
    void mark_all_running_as_se_on_shutdown(std::string_view) override {}
    std::optional<Submission> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : rows_) if (s.id == id) return s;
        return std::nullopt;
    }
    std::optional<SubmissionDetail> get_full(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::optional<Submission> sub;
        for (const auto& s : rows_) if (s.id == id) { sub = s; break; }
        if (!sub.has_value()) return std::nullopt;
        SubmissionDetail d;
        d.submission = *sub;
        d.username   = (sub->user_id == 1) ? "alice" :
                       (sub->user_id == 2) ? "bob"   : "user" + std::to_string(sub->user_id);
        for (const auto& c : cases_) {
            if (c.submission_id == id) d.cases.push_back(c);
        }
        std::sort(d.cases.begin(), d.cases.end(),
                  [](const auto& a, const auto& b) {
                      return a.case_index < b.case_index;
                  });
        return d;
    }
    SubmissionListResult list_by_user(const SubmissionListQuery&) override { return {}; }
    SubmissionListResult list_public_accepted(const SubmissionListQuery&) override { return {}; }
private:
    std::mutex                       mu_;
    std::vector<Submission>          rows_;
    std::vector<SubmissionCase>      cases_;
    std::int64_t                     next_id_{0};
};

class InMemoryProblemRepo : public IProblemRepository {
public:
    void add(Problem p) {
        std::lock_guard<std::mutex> lk(mu_);
        ++next_id_;
        p.id = next_id_;
        rows_.push_back(p);
    }
    std::optional<Problem> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& p : rows_) if (p.id == id) return p;
        return std::nullopt;
    }
    ProblemListResult list(const ProblemListQuery&) override { return {}; }
    Problem create(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto copy = p;
        copy.id = ++next_id_;
        rows_.push_back(copy);
        return copy;
    }
    void update(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& x : rows_) if (x.id == p.id) x = p;
    }
    void soft_delete(std::int64_t) override {}
    void set_published(std::int64_t, bool) override {}
    std::pair<int, int> submission_stats(std::int64_t) override { return {0, 0}; }
private:
    std::mutex                mu_;
    std::vector<Problem>      rows_;
    std::int64_t              next_id_{0};
};

class InMemoryTestcaseRepo : public ITestcaseRepository {
public:
    void add(Testcase t) { std::lock_guard<std::mutex> lk(mu_); tcs_.push_back(std::move(t)); }
    std::vector<Testcase> list_by_problem(std::int64_t) override { return {}; }
    std::vector<Testcase> list_samples(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Testcase> out;
        for (const auto& t : tcs_) if (t.problem_id == pid && t.is_sample) out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
    void create_many(std::int64_t, const std::vector<Testcase>&) override {}
    void replace_by_problem(std::int64_t, const std::vector<Testcase>&) override {}
    void delete_by_problem(std::int64_t) override {}

    // 测试钩
    void set_throw(bool v) { throw_ = v; }
private:
    std::mutex              mu_;
    std::vector<Testcase>   tcs_;
    bool                    throw_{false};
};

class ThrowingTestcaseRepo : public ITestcaseRepository {
public:
    std::vector<Testcase> list_by_problem(std::int64_t) override { return {}; }
    std::vector<Testcase> list_samples(std::int64_t) override {
        throw std::runtime_error("synthetic testcase repo failure");
    }
    void create_many(std::int64_t, const std::vector<Testcase>&) override {}
    void replace_by_problem(std::int64_t, const std::vector<Testcase>&) override {}
    void delete_by_problem(std::int64_t) override {}
};

Problem mk_problem(bool published) {
    Problem p;
    p.title        = "p";
    p.content_md   = "x";
    p.difficulty   = Difficulty::Easy;
    p.is_published = published;
    p.created_by   = 1;
    return p;
}

// ===========================================================================
//  错误信息内容
// ===========================================================================
TEST(SubmissionServiceEdgeTest, ErrorMessageNegativeProblemId) {
    auto svc = std::make_shared<SubmissionService>(
        std::make_shared<InMemorySubmissionRepo>(),
        std::make_shared<InMemoryProblemRepo>(),
        std::make_shared<InMemoryTestcaseRepo>(),
        65536);
    try { svc->create(1, -1, Language::Cpp, "x"); FAIL() << "expected throw"; }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("problem_id"), std::string::npos);
    }
}

TEST(SubmissionServiceEdgeTest, ErrorMessageEmptyCode) {
    auto svc = std::make_shared<SubmissionService>(
        std::make_shared<InMemorySubmissionRepo>(),
        std::make_shared<InMemoryProblemRepo>(),
        std::make_shared<InMemoryTestcaseRepo>(),
        65536);
    // 先得有题目
    auto problems = std::make_shared<InMemoryProblemRepo>();
    problems->add(mk_problem(true));
    auto sub = std::make_shared<InMemorySubmissionRepo>();
    auto tc  = std::make_shared<InMemoryTestcaseRepo>();
    auto svc2 = std::make_shared<SubmissionService>(sub, problems, tc, 65536);
    try { svc2->create(1, 1, Language::Cpp, ""); FAIL() << "expected throw"; }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("empty"), std::string::npos);
    }
}

TEST(SubmissionServiceEdgeTest, ErrorMessageCodeTooLargeMentionsBytes) {
    auto problems = std::make_shared<InMemoryProblemRepo>();
    problems->add(mk_problem(true));
    auto svc = std::make_shared<SubmissionService>(
        std::make_shared<InMemorySubmissionRepo>(), problems,
        std::make_shared<InMemoryTestcaseRepo>(), /*code_max=*/100);
    try { svc->create(1, 1, Language::Cpp, std::string(200, 'a')); FAIL() << "expected throw"; }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::TooLarge);
        const std::string m = e.what();
        EXPECT_NE(m.find("200"),  std::string::npos);
        EXPECT_NE(m.find("100"),  std::string::npos);
    }
}

TEST(SubmissionServiceEdgeTest, ErrorMessageProblemNotFoundMentionsProblem) {
    auto svc = std::make_shared<SubmissionService>(
        std::make_shared<InMemorySubmissionRepo>(),
        std::make_shared<InMemoryProblemRepo>(),
        std::make_shared<InMemoryTestcaseRepo>(),
        65536);
    try { svc->create(1, 999, Language::Cpp, "x"); FAIL() << "expected throw"; }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::NotFound);
        EXPECT_NE(std::string{e.what()}.find("problem"), std::string::npos);
    }
}

// ===========================================================================
//  code_max_bytes getter
// ===========================================================================
TEST(SubmissionServiceEdgeTest, CodeMaxBytesGetter) {
    auto svc = std::make_shared<SubmissionService>(
        std::make_shared<InMemorySubmissionRepo>(),
        std::make_shared<InMemoryProblemRepo>(),
        std::make_shared<InMemoryTestcaseRepo>(),
        /*code_max=*/42);
    EXPECT_EQ(svc->code_max_bytes(), 42);
}

// ===========================================================================
//  get_detail 降级路径
// ===========================================================================
TEST(SubmissionServiceEdgeTest, GetDetailWorksWhenTestcasesRepoIsNull) {
    // 用 nullptr 构造 service —— get_detail 仍能跑（不调 testcases_ 即可）
    auto sub  = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    SubmissionService svc(sub, probs, /*testcases=*/nullptr, 65536);
    auto id = svc.create(1, 1, Language::Cpp, "x");
    sub->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");

    auto d = svc.get_detail(id, 0, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.id, id);
    EXPECT_EQ(d->username, "alice");
    // 没有 testcase repo → cases 数组里 input/expected 留空
    if (!d->cases.empty()) {
        for (const auto& c : d->cases) {
            EXPECT_TRUE(c.input.empty());
            EXPECT_TRUE(c.expected_output.empty());
        }
    }
}

TEST(SubmissionServiceEdgeTest, GetDetailIgnoresTestcaseRepoExceptions) {
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<ThrowingTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);
    auto id = svc.create(1, 1, Language::Cpp, "x");
    sub->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");

    // 不应该抛 —— list_samples 异常被吞掉
    auto d = svc.get_detail(id, 0, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.id, id);
}

TEST(SubmissionServiceEdgeTest, GetDetailWithNoTestcasesAtAll) {
    // 题目存在但 testcases 表为空
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);
    auto id = svc.create(1, 1, Language::Cpp, "x");
    sub->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");

    auto d = svc.get_detail(id, 0, false);
    ASSERT_TRUE(d.has_value());
    // 没有任何 case → cases 数组空
    EXPECT_TRUE(d->cases.empty());
}

TEST(SubmissionServiceEdgeTest, GetDetailWithSubmissionButNoCases) {
    // 提交记录存在但 submission_cases 表里没有行（判题还没落库）
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);
    auto id = svc.create(1, 1, Language::Cpp, "x");
    // 不调 finish → 状态保持 Queued，cases 也空
    auto d = svc.get_detail(id, 1, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.status, SubmissionStatus::Queued);
    EXPECT_TRUE(d->cases.empty());
}

// ===========================================================================
//  username 字段
// ===========================================================================
TEST(SubmissionServiceEdgeTest, UsernamePropagatedFromRepo) {
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);

    // user 1 → alice
    auto id1 = svc.create(1, 1, Language::Cpp, "x");
    sub->finish(id1, SubmissionResult::AC, 100, 10, 1024, "", "");
    auto d1 = svc.get_detail(id1, 1, false);
    ASSERT_TRUE(d1.has_value());
    EXPECT_EQ(d1->username, "alice");

    // user 2 → bob
    auto id2 = svc.create(2, 1, Language::Cpp, "y");
    sub->finish(id2, SubmissionResult::AC, 100, 10, 1024, "", "");
    auto d2 = svc.get_detail(id2, 2, false);
    ASSERT_TRUE(d2.has_value());
    EXPECT_EQ(d2->username, "bob");
}

// ===========================================================================
//  多提交 / 跨用户独立性
// ===========================================================================
TEST(SubmissionServiceEdgeTest, MultipleSubmissionsForSameProblemAreIndependent) {
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);

    auto id1 = svc.create(1, 1, Language::Cpp, "code-1");
    auto id2 = svc.create(1, 1, Language::Cpp, "code-2");
    EXPECT_NE(id1, id2);

    auto s1 = sub->find_by_id(id1);
    auto s2 = sub->find_by_id(id2);
    ASSERT_TRUE(s1.has_value() && s2.has_value());
    EXPECT_EQ(s1->code, "code-1");
    EXPECT_EQ(s2->code, "code-2");
    EXPECT_EQ(s1->user_id, s2->user_id);
}

TEST(SubmissionServiceEdgeTest, DifferentUsersSameProblemVisibility) {
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);

    // user 1 提交一道 WA
    auto id_user1 = svc.create(1, 1, Language::Cpp, "x");
    sub->finish(id_user1, SubmissionResult::WA, 30, 10, 1024, "", "");

    // user 2 看 user 1 的 WA → Forbidden
    EXPECT_THROW(svc.get_detail(id_user1, /*requester_id=*/2, false), GetSubmissionError);
    // user 1 自己看 → OK
    auto d = svc.get_detail(id_user1, 1, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.id, id_user1);
}

// ===========================================================================
//  异常传播
// ===========================================================================
TEST(SubmissionServiceEdgeTest, GetDetailSurfacesRepoErrorAsInternal) {
    struct ThrowingRepo : public ISubmissionRepository {
        std::int64_t create(std::int64_t, std::int64_t, Language, std::string_view) override { return 0; }
        bool claim_one(ClaimedTask&) override { return false; }
        JudgeTaskPayload load_task(std::int64_t) override { return {}; }
        void update_status(std::int64_t, SubmissionStatus) override {}
        void finish(std::int64_t, SubmissionResult, int, int, int,
                    std::string_view, std::string_view) override {}
        void insert_case(std::int64_t, const SubmissionCase&) override {}
        void mark_all_running_as_se_on_shutdown(std::string_view) override {}
        std::optional<Submission> find_by_id(std::int64_t) override { return std::nullopt; }
        std::optional<SubmissionDetail> get_full(std::int64_t) override {
            throw std::runtime_error("synthetic get_full failure");
        }
        SubmissionListResult list_by_user(const SubmissionListQuery&) override { return {}; }
        SubmissionListResult list_public_accepted(const SubmissionListQuery&) override { return {}; }
    };
    auto sub   = std::make_shared<ThrowingRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);
    try { svc.get_detail(1, 0, false); FAIL() << "expected throw"; }
    catch (const GetSubmissionError& e) {
        EXPECT_EQ(e.kind(), GetSubmissionErrorKind::Internal);
    }
}

TEST(SubmissionServiceEdgeTest, CreateSurfacesProblemRepoErrorAsInternal) {
    struct ThrowingProblemRepo : public IProblemRepository {
        std::optional<Problem> find_by_id(std::int64_t) override {
            throw std::runtime_error("synthetic find_by_id failure");
        }
        ProblemListResult list(const ProblemListQuery&) override { return {}; }
        Problem create(const Problem& p) override { return p; }
        void update(const Problem&) override {}
        void soft_delete(std::int64_t) override {}
        void set_published(std::int64_t, bool) override {}
        std::pair<int, int> submission_stats(std::int64_t) override { return {0, 0}; }
    };
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<ThrowingProblemRepo>();
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);
    try { svc.create(1, 1, Language::Cpp, "x"); FAIL() << "expected throw"; }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::Internal);
    }
}

// ===========================================================================
//  result 类型全覆盖（除 AC / WA 之外的 6 个状态在 get_detail 上的可见性）
// ===========================================================================
TEST(SubmissionServiceEdgeTest, AllNonACResultsInvisibleToNonOwner) {
    auto sub   = std::make_shared<InMemorySubmissionRepo>();
    auto probs = std::make_shared<InMemoryProblemRepo>();
    probs->add(mk_problem(true));
    auto tc    = std::make_shared<InMemoryTestcaseRepo>();
    SubmissionService svc(sub, probs, tc, 65536);

    const SubmissionResult results[] = {
        SubmissionResult::WA, SubmissionResult::TLE, SubmissionResult::MLE,
        SubmissionResult::OLE, SubmissionResult::RE, SubmissionResult::CE,
        SubmissionResult::SE,
    };
    for (auto r : results) {
        auto id = svc.create(1, 1, Language::Cpp, "x");
        sub->finish(id, r, 0, 0, 0, "", "");
        // 非 owner 匿名 → Forbidden
        try { svc.get_detail(id, 0, false); FAIL() << "expected throw for " << oj::domain::to_string(r); }
        catch (const GetSubmissionError& e) {
            EXPECT_EQ(e.kind(), GetSubmissionErrorKind::Forbidden) << "result=" << oj::domain::to_string(r);
        }
    }
}

}  // namespace
