// =============================================================================
//  test_submission_service.cpp —— SubmissionService 单元测试
//  用 in-memory repo 替换 MySQL，覆盖 SPEC §2.3 / §5.2.3 业务规则：
//    - create 字段校验（problem_id / code size / language 通过）
//    - create 题目不存在 / 未发布 → NotFound
//    - create happy path 返回 id
//    - get_detail 可见性：AC 公开；非 AC 仅 owner/admin；不可见 → Forbidden
//    - get_detail 样例点回填 input/expected_output
//    - get_detail 找不到 → nullopt
//    - create 内部异常 → Internal
//    - get_detail 内部异常 → Internal
// =============================================================================

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
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
//  In-memory ISubmissionRepository
// ---------------------------------------------------------------------------
class InMemorySubmissionRepo : public ISubmissionRepository {
public:
    std::int64_t create(std::int64_t user_id, std::int64_t problem_id,
                        Language language, std::string_view code) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (throw_on_create_) throw std::runtime_error("synthetic create failure");
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
    void update_status(std::int64_t id, SubmissionStatus st) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) if (s.id == id) s.status = st;
    }
    void finish(std::int64_t id, SubmissionResult result, int score, int time_ms,
                int mem_kb, std::string_view co, std::string_view jm) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) {
            if (s.id == id) {
                s.status           = SubmissionStatus::Finished;
                s.result           = result;
                s.total_score      = score;
                s.time_used_ms     = time_ms;
                s.memory_used_kb   = mem_kb;
                s.compile_output   = std::string{co};
                s.judge_message    = std::string{jm};
                s.finished_at      = "2026-06-17T00:00:01Z";
            }
        }
    }
    void insert_case(std::int64_t submission_id, const SubmissionCase& c) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto sc = c;
        sc.submission_id = submission_id;
        cases_.push_back(sc);
    }
    void mark_all_running_as_se_on_shutdown(std::string_view) override {}

    std::optional<Submission> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& s : rows_) if (s.id == id) return s;
        return std::nullopt;
    }

    std::optional<SubmissionDetail> get_full(std::int64_t id) override {
        if (throw_on_get_full_) throw std::runtime_error("synthetic get_full failure");
        std::lock_guard<std::mutex> lk(mu_);
        // 直接查 row（find_by_id 会再次加锁，会死锁）
        std::optional<Submission> sub;
        for (const auto& s : rows_) if (s.id == id) { sub = s; break; }
        if (!sub.has_value()) return std::nullopt;
        SubmissionDetail d;
        d.submission = *sub;
        d.username   = (sub->user_id == 1) ? "alice" : "bob";
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

    // 测试钩：注入失败
    void set_throw_on_create(bool v)    { throw_on_create_    = v; }
    void set_throw_on_get_full(bool v)  { throw_on_get_full_  = v; }
    void add_case(std::int64_t sub_id, SubmissionCase c) {
        std::lock_guard<std::mutex> lk(mu_);
        c.submission_id = sub_id;
        cases_.push_back(std::move(c));
    }

private:
    std::mutex                       mu_;
    std::vector<Submission>          rows_;
    std::vector<SubmissionCase>      cases_;
    std::int64_t                     next_id_{0};
    bool                             throw_on_create_{false};
    bool                             throw_on_get_full_{false};
};

// ---------------------------------------------------------------------------
//  In-memory IProblemRepository
// ---------------------------------------------------------------------------
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
    void set_published(std::int64_t id, bool pub) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& p : rows_) if (p.id == id) p.is_published = pub;
    }
    std::pair<int, int> submission_stats(std::int64_t) override { return {0, 0}; }

    // 测试钩
    void set_throw(bool v) { throw_ = v; }

private:
    std::mutex                mu_;
    std::vector<Problem>      rows_;
    std::int64_t              next_id_{0};
    bool                      throw_{false};
};

// ---------------------------------------------------------------------------
//  In-memory ITestcaseRepository
// ---------------------------------------------------------------------------
class InMemoryTestcaseRepo : public ITestcaseRepository {
public:
    void add(Testcase t) {
        std::lock_guard<std::mutex> lk(mu_);
        tcs_.push_back(std::move(t));
    }
    std::vector<Testcase> list_by_problem(std::int64_t pid) override {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Testcase> out;
        for (const auto& t : tcs_) if (t.problem_id == pid) out.push_back(t);
        std::sort(out.begin(), out.end(),
                  [](const auto& a, const auto& b) { return a.case_index < b.case_index; });
        return out;
    }
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

private:
    std::mutex              mu_;
    std::vector<Testcase>   tcs_;
};

// ---------------------------------------------------------------------------
//  工厂
// ---------------------------------------------------------------------------
struct ServiceBundle {
    std::shared_ptr<InMemorySubmissionRepo>  submissions;
    std::shared_ptr<InMemoryProblemRepo>     problems;
    std::shared_ptr<InMemoryTestcaseRepo>    testcases;
    std::shared_ptr<SubmissionService>       service;
};

ServiceBundle make_service(int code_max = 65536) {
    ServiceBundle b;
    b.submissions = std::make_shared<InMemorySubmissionRepo>();
    b.problems    = std::make_shared<InMemoryProblemRepo>();
    b.testcases   = std::make_shared<InMemoryTestcaseRepo>();
    b.service     = std::make_shared<SubmissionService>(
        b.submissions, b.problems, b.testcases, code_max);
    return b;
}

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
//  create —— 字段校验
// ===========================================================================
TEST(SubmissionServiceTest, CreateRejectsNegativeProblemId) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    EXPECT_THROW(b.service->create(1, -1, Language::Cpp, "x"),
                 CreateSubmissionError);
    try { b.service->create(1, -1, Language::Cpp, "x"); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::BadRequest);
    }
}

TEST(SubmissionServiceTest, CreateRejectsZeroProblemId) {
    auto b = make_service();
    EXPECT_THROW(b.service->create(1, 0, Language::Cpp, "x"),
                 CreateSubmissionError);
}

TEST(SubmissionServiceTest, CreateRejectsEmptyCode) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    EXPECT_THROW(b.service->create(1, 1, Language::Cpp, ""),
                 CreateSubmissionError);
    try { b.service->create(1, 1, Language::Cpp, ""); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::BadRequest);
    }
}

TEST(SubmissionServiceTest, CreateRejectsCodeLargerThanMax) {
    auto b = make_service(/*code_max=*/10);
    b.problems->add(mk_problem(true));
    EXPECT_THROW(b.service->create(1, 1, Language::Cpp, "this-is-too-long"),
                 CreateSubmissionError);
    try { b.service->create(1, 1, Language::Cpp, "this-is-too-long"); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::TooLarge);
    }
}

TEST(SubmissionServiceTest, CreateAcceptsCodeExactlyAtLimit) {
    auto b = make_service(/*code_max=*/10);
    b.problems->add(mk_problem(true));
    EXPECT_NO_THROW(b.service->create(1, 1, Language::Cpp, "1234567890"));
}

TEST(SubmissionServiceTest, CreateRejectsProblemNotFound) {
    auto b = make_service();
    // 题目表为空
    EXPECT_THROW(b.service->create(1, 999, Language::Cpp, "x"),
                 CreateSubmissionError);
    try { b.service->create(1, 999, Language::Cpp, "x"); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::NotFound);
    }
}

TEST(SubmissionServiceTest, CreateRejectsUnpublishedProblem) {
    auto b = make_service();
    b.problems->add(mk_problem(/*published=*/false));
    EXPECT_THROW(b.service->create(1, 1, Language::Cpp, "x"),
                 CreateSubmissionError);
    try { b.service->create(1, 1, Language::Cpp, "x"); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::NotFound);
    }
}

TEST(SubmissionServiceTest, CreateHappyPath) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(/*user_id=*/7, /*problem_id=*/1,
                                 Language::Cpp, "int main(){}");
    EXPECT_GT(id, 0);
    auto sub = b.submissions->find_by_id(id);
    ASSERT_TRUE(sub.has_value());
    EXPECT_EQ(sub->user_id,    7);
    EXPECT_EQ(sub->problem_id, 1);
    EXPECT_EQ(sub->language,   Language::Cpp);
    EXPECT_EQ(sub->code,       "int main(){}");
    EXPECT_EQ(sub->status,     SubmissionStatus::Queued);
}

TEST(SubmissionServiceTest, CreateSurfacesRepoErrorAsInternal) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    b.submissions->set_throw_on_create(true);
    EXPECT_THROW(b.service->create(1, 1, Language::Cpp, "x"),
                 CreateSubmissionError);
    try { b.service->create(1, 1, Language::Cpp, "x"); }
    catch (const CreateSubmissionError& e) {
        EXPECT_EQ(e.kind(), CreateSubmissionErrorKind::Internal);
    }
}

// ===========================================================================
//  get_detail —— 找不到 / 可见性 / 样例回填
// ===========================================================================
TEST(SubmissionServiceTest, GetDetailReturnsNulloptWhenMissing) {
    auto b = make_service();
    auto d = b.service->get_detail(123, /*requester_id=*/0, /*is_admin=*/false);
    EXPECT_FALSE(d.has_value());
}

TEST(SubmissionServiceTest, GetDetailSurfacesRepoErrorAsInternal) {
    auto b = make_service();
    b.submissions->set_throw_on_get_full(true);
    EXPECT_THROW(b.service->get_detail(1, 0, false), GetSubmissionError);
    try { b.service->get_detail(1, 0, false); }
    catch (const GetSubmissionError& e) {
        EXPECT_EQ(e.kind(), GetSubmissionErrorKind::Internal);
    }
}

TEST(SubmissionServiceTest, GetDetailAnonymousSeesACSubmission) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(1, 1, Language::Cpp, "x");
    // 模拟判题完成 → AC
    b.submissions->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");

    auto d = b.service->get_detail(id, /*requester_id=*/0, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.id, id);
    EXPECT_EQ(*d->submission.result, SubmissionResult::AC);
    EXPECT_EQ(d->username, "alice");
}

TEST(SubmissionServiceTest, GetDetailAnonymousCannotSeeWASubmission) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::WA, 60, 10, 1024, "", "");

    EXPECT_THROW(b.service->get_detail(id, /*requester_id=*/0, false),
                 GetSubmissionError);
    try { b.service->get_detail(id, 0, false); }
    catch (const GetSubmissionError& e) {
        EXPECT_EQ(e.kind(), GetSubmissionErrorKind::Forbidden);
    }
}

TEST(SubmissionServiceTest, GetDetailOtherUserCannotSeeWASubmission) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(/*user_id=*/1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::WA, 60, 10, 1024, "", "");

    // user_id=2 不是 owner
    EXPECT_THROW(b.service->get_detail(id, /*requester_id=*/2, false),
                 GetSubmissionError);
}

TEST(SubmissionServiceTest, GetDetailOwnerCanSeeOwnWASubmission) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(/*user_id=*/1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::WA, 60, 10, 1024, "", "");

    auto d = b.service->get_detail(id, /*requester_id=*/1, false);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(d->submission.id, id);
    EXPECT_EQ(*d->submission.result, SubmissionResult::WA);
}

TEST(SubmissionServiceTest, GetDetailAdminCanSeeAllSubmissions) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(/*user_id=*/1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::TLE, 30, 2000, 1024, "", "");

    // admin 看其他人的非 AC → 可见
    auto d = b.service->get_detail(id, /*requester_id=*/2, /*is_admin=*/true);
    ASSERT_TRUE(d.has_value());
    EXPECT_EQ(*d->submission.result, SubmissionResult::TLE);
}

TEST(SubmissionServiceTest, GetDetailUnfinishedVisibleOnlyToOwnerOrAdmin) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(/*user_id=*/1, 1, Language::Cpp, "x");
    // 状态保持 Queued —— 不调 finish

    // 匿名：不可见
    EXPECT_THROW(b.service->get_detail(id, 0, false), GetSubmissionError);
    // 非 owner：不可见
    EXPECT_THROW(b.service->get_detail(id, 2, false), GetSubmissionError);
    // owner：可见
    auto d1 = b.service->get_detail(id, 1, false);
    ASSERT_TRUE(d1.has_value());
    EXPECT_EQ(d1->submission.status, SubmissionStatus::Queued);
    // admin：可见
    auto d2 = b.service->get_detail(id, 2, /*is_admin=*/true);
    ASSERT_TRUE(d2.has_value());
}

TEST(SubmissionServiceTest, GetDetailFillsSampleCaseInputAndExpected) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id = b.service->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::WA, 30, 10, 1024, "", "");

    // 加一个样例点
    Testcase tc;
    tc.problem_id      = 1;
    tc.case_index      = 1;
    tc.input           = "1 2\n";
    tc.expected_output = "3\n";
    tc.is_sample       = true;
    tc.score           = 30;
    b.testcases->add(tc);

    // 加一个隐藏点
    Testcase hc;
    hc.problem_id      = 1;
    hc.case_index      = 2;
    hc.input           = "1 2\n";
    hc.expected_output = "3\n";
    hc.is_sample       = false;
    hc.score           = 70;
    b.testcases->add(hc);

    // 加 submission_cases
    SubmissionCase sc1;
    sc1.case_index = 1;
    sc1.is_sample  = true;
    sc1.user_output = "1 2\n";
    sc1.score       = 0;
    sc1.status      = SubmissionResult::WA;
    b.submissions->add_case(id, sc1);

    SubmissionCase sc2;
    sc2.case_index = 2;
    sc2.is_sample  = false;
    sc2.user_output.clear();
    sc2.score       = 30;
    sc2.status      = SubmissionResult::AC;
    b.submissions->add_case(id, sc2);

    auto d = b.service->get_detail(id, /*requester_id=*/1, false);
    ASSERT_TRUE(d.has_value());
    ASSERT_EQ(d->cases.size(), 2u);

    // case 1（样例点）→ 应该有 input/expected_output
    EXPECT_EQ(d->cases[0].input,           "1 2\n");
    EXPECT_EQ(d->cases[0].expected_output, "3\n");
    EXPECT_EQ(d->cases[0].user_output,     "1 2\n");

    // case 2（隐藏点）→ input/expected/user_output 都空
    EXPECT_TRUE(d->cases[1].input.empty());
    EXPECT_TRUE(d->cases[1].expected_output.empty());
    EXPECT_TRUE(d->cases[1].user_output.empty());
}

}  // namespace
