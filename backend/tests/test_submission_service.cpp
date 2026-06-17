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
#include <set>
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
    SubmissionListResult list_by_user(const SubmissionListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        return build_list(q, /*public_only=*/false, /*user_id_override=*/0);
    }
    SubmissionListResult list_public_accepted(const SubmissionListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        return build_list(q, /*public_only=*/true, /*user_id_override=*/0);
    }

    // 共享 list 实现
    SubmissionListResult build_list(const SubmissionListQuery& q,
                                    bool public_only,
                                    std::int64_t user_id_override) {
        if (throw_on_list_) throw std::runtime_error("synthetic list failure");
        SubmissionListResult r;
        r.page      = q.page;
        r.page_size = q.page_size;
        std::vector<Submission> matched;
        for (const auto& s : rows_) {
            if (public_only) {
                if (s.status != SubmissionStatus::Finished) continue;
                if (!s.result.has_value() || *s.result != SubmissionResult::AC) continue;
            }
            if (user_id_override > 0 && s.user_id != user_id_override) continue;
            if (q.user_id > 0 && s.user_id != q.user_id) continue;
            if (q.problem_id > 0 && s.problem_id != q.problem_id) continue;
            if (q.language.has_value() && s.language != *q.language) continue;
            if (q.status.has_value() && s.status != *q.status) continue;
            matched.push_back(s);
        }
        r.total = static_cast<std::int64_t>(matched.size());
        std::sort(matched.begin(), matched.end(),
                  [](const Submission& a, const Submission& b) {
                      if (a.created_at != b.created_at) return a.created_at > b.created_at;
                      return a.id > b.id;
                  });
        const int offset = (q.page - 1) * q.page_size;
        if (offset >= static_cast<int>(matched.size())) return r;
        for (std::size_t i = offset; i < matched.size() && static_cast<int>(r.items.size()) < q.page_size; ++i) {
            const Submission& s = matched[i];
            oj::domain::SubmissionListItem it;
            it.id              = s.id;
            it.user_id         = s.user_id;
            it.problem_id      = s.problem_id;
            it.problem_title   = problem_title_of(s.problem_id);
            it.username        = (s.user_id == 1) ? "alice" : (s.user_id == 2) ? "bob" : std::string{};
            it.language        = s.language;
            it.status          = s.status;
            it.result          = s.result;
            it.total_score     = s.total_score;
            it.time_used_ms    = s.time_used_ms;
            it.memory_used_kb  = s.memory_used_kb;
            it.created_at      = s.created_at;
            it.finished_at     = s.finished_at;
            r.items.push_back(std::move(it));
        }
        return r;
    }

    // 测试钩：注入失败
    void set_throw_on_create(bool v)    { throw_on_create_    = v; }
    void set_throw_on_get_full(bool v)  { throw_on_get_full_  = v; }
    void set_throw_on_list(bool v)      { throw_on_list_      = v; }
    void set_problem_title(std::int64_t problem_id, std::string title) {
        std::lock_guard<std::mutex> lk(mu_);
        problem_titles_[problem_id] = std::move(title);
    }
    std::string problem_title_of(std::int64_t problem_id) const {
        // 不加锁（仅在已持锁处调用）
        auto it = problem_titles_.find(problem_id);
        return it != problem_titles_.end() ? it->second : std::string{};
    }
    void add_case(std::int64_t sub_id, SubmissionCase c) {
        std::lock_guard<std::mutex> lk(mu_);
        c.submission_id = sub_id;
        cases_.push_back(std::move(c));
    }
    void set_created_at(std::int64_t sub_id, const std::string& t) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) if (s.id == sub_id) s.created_at = t;
    }

private:
    std::mutex                       mu_;
    std::vector<Submission>          rows_;
    std::vector<SubmissionCase>      cases_;
    std::int64_t                     next_id_{0};
    bool                             throw_on_create_{false};
    bool                             throw_on_get_full_{false};
    bool                             throw_on_list_{false};
    std::map<std::int64_t, std::string> problem_titles_;
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

// 简单的回调：让 InMemoryProblemRepo add() 时把 (id, title) 推给 InMemorySubmissionRepo
// —— 模拟 SQL JOIN 行为。
static void wire_problem_titles(InMemoryProblemRepo* p, InMemorySubmissionRepo* s) {
    // 测试通过 ServiceBundle.problems->add(p) 添加题目；
    // 我们用 hook：add 之后 p->rows_ 已经更新了 title
    // 简化做法：提供一个专门的 set_problem_title 接口供测试用
    (void)p; (void)s;
}

ServiceBundle make_service(int code_max = 65536) {
    ServiceBundle b;
    b.submissions = std::make_shared<InMemorySubmissionRepo>();
    b.problems    = std::make_shared<InMemoryProblemRepo>();
    b.testcases   = std::make_shared<InMemoryTestcaseRepo>();
    wire_problem_titles(b.problems.get(), b.submissions.get());
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

// ===========================================================================
//  list_by_user —— 强制 user_id = requester_id（个人列表只能是本人）
// ===========================================================================
TEST(SubmissionServiceTest, ListByUserRequiresValidRequesterId) {
    auto b = make_service();
    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    EXPECT_THROW(b.service->list_by_user(/*requester_id=*/0, q),
                 std::runtime_error);
    EXPECT_THROW(b.service->list_by_user(/*requester_id=*/-1, q),
                 std::runtime_error);
}

TEST(SubmissionServiceTest, ListByUserIgnoresQueryUserIdAndForcesRequesterId) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    // 2 个 user_id=1 的 submission，1 个 user_id=2 的 submission
    auto id1 = b.service->create(1, 1, Language::Cpp, "a");
    auto id2 = b.service->create(1, 1, Language::Cpp, "b");
    auto id3 = b.service->create(2, 1, Language::Cpp, "c");

    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    q.user_id = 2;    // 试图用 user_id=2 偷看 user_id=1 的列表

    auto r = b.service->list_by_user(/*requester_id=*/1, q);
    EXPECT_EQ(r.total, 2);
    for (const auto& it : r.items) {
        EXPECT_EQ(it.user_id, 1);   // 强制覆盖：只返回 user_id=1 的
    }

    // user_id=2 看自己的
    auto r2 = b.service->list_by_user(/*requester_id=*/2, q);
    EXPECT_EQ(r2.total, 1);
    EXPECT_EQ(r2.items[0].user_id, 2);
}

TEST(SubmissionServiceTest, ListByUserReturnsFieldsWithProblemTitleAndUsername) {
    auto b = make_service();
    auto p = mk_problem(true);
    p.title = "两数之和";
    b.problems->add(p);
    // 把 title 同步给 submission repo（模拟 SQL JOIN）
    // 注意：service->create() 用的是 problem_id=1，repo->add() 后 p.id 也是 1
    b.submissions->set_problem_title(1, "两数之和");
    auto id = b.service->create(1, 1, Language::Java, "x");
    b.submissions->finish(id, SubmissionResult::AC, 100, 12, 2048, "", "");

    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = b.service->list_by_user(1, q);
    ASSERT_EQ(r.items.size(), 1u);
    EXPECT_EQ(r.items[0].id,             id);
    EXPECT_EQ(r.items[0].user_id,        1);
    EXPECT_EQ(r.items[0].problem_id,     1);
    EXPECT_EQ(r.items[0].problem_title,  "两数之和");
    EXPECT_EQ(r.items[0].username,       "alice");
    EXPECT_EQ(r.items[0].language,       Language::Java);
    EXPECT_EQ(r.items[0].status,         SubmissionStatus::Finished);
    ASSERT_TRUE(r.items[0].result.has_value());
    EXPECT_EQ(*r.items[0].result,        SubmissionResult::AC);
    EXPECT_EQ(r.items[0].total_score,    100);
    EXPECT_EQ(r.items[0].time_used_ms,   12);
    EXPECT_EQ(r.items[0].memory_used_kb, 2048);
}

TEST(SubmissionServiceTest, ListByUserFiltersByLanguageAndStatus) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id1 = b.service->create(1, 1, Language::Cpp,    "a");
    auto id2 = b.service->create(1, 1, Language::Java,   "b");
    auto id3 = b.service->create(1, 1, Language::Python, "c");
    b.submissions->finish(id1, SubmissionResult::AC,  100, 1, 1, "", "");
    // id2 保持 Queued
    b.submissions->finish(id3, SubmissionResult::WA,  60,  1, 1, "", "");

    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = b.service->list_by_user(1, q);
    EXPECT_EQ(r.total, 3);

    q.language = Language::Cpp;
    auto r2 = b.service->list_by_user(1, q);
    EXPECT_EQ(r2.total, 1);
    EXPECT_EQ(r2.items[0].language, Language::Cpp);

    q = SubmissionListQuery{};
    q.page = 1; q.page_size = 10;
    q.status = SubmissionStatus::Queued;
    auto r3 = b.service->list_by_user(1, q);
    EXPECT_EQ(r3.total, 1);
    EXPECT_EQ(r3.items[0].status, SubmissionStatus::Queued);
}

TEST(SubmissionServiceTest, ListByUserSurfacesRepoError) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    b.submissions->set_throw_on_list(true);
    SubmissionListQuery q;
    EXPECT_THROW(b.service->list_by_user(1, q), std::exception);
}

// ===========================================================================
//  list_public_accepted —— 只看 result=AC + status=finished 的
// ===========================================================================
TEST(SubmissionServiceTest, ListPublicAcceptedExcludesNonAC) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    // 2 AC + 1 WA + 1 未完成
    auto id1 = b.service->create(1, 1, Language::Cpp, "a");
    auto id2 = b.service->create(2, 1, Language::Cpp, "b");
    auto id3 = b.service->create(1, 1, Language::Cpp, "c");
    b.submissions->finish(id1, SubmissionResult::AC, 100, 1, 1, "", "");
    b.submissions->finish(id2, SubmissionResult::AC, 100, 1, 1, "", "");
    b.submissions->finish(id3, SubmissionResult::WA,  60,  1, 1, "", "");

    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = b.service->list_public_accepted(q);
    EXPECT_EQ(r.total, 2);
    for (const auto& it : r.items) {
        ASSERT_TRUE(it.result.has_value());
        EXPECT_EQ(*it.result, SubmissionResult::AC);
        EXPECT_EQ(it.status, SubmissionStatus::Finished);
    }
}

TEST(SubmissionServiceTest, ListPublicAcceptedDoesNotFilterByUserId) {
    auto b = make_service();
    b.problems->add(mk_problem(true));
    auto id1 = b.service->create(1, 1, Language::Cpp, "a");
    auto id2 = b.service->create(2, 1, Language::Cpp, "b");
    b.submissions->finish(id1, SubmissionResult::AC, 100, 1, 1, "", "");
    b.submissions->finish(id2, SubmissionResult::AC, 100, 1, 1, "", "");

    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = b.service->list_public_accepted(q);
    EXPECT_EQ(r.total, 2);
    // 来自两个不同 user
    std::set<std::int64_t> uids;
    for (const auto& it : r.items) uids.insert(it.user_id);
    EXPECT_EQ(uids.size(), 2u);
}

TEST(SubmissionServiceTest, ListPublicAcceptedSurfacesRepoError) {
    auto b = make_service();
    b.submissions->set_throw_on_list(true);
    SubmissionListQuery q;
    EXPECT_THROW(b.service->list_public_accepted(q), std::exception);
}

TEST(SubmissionServiceTest, ListPublicAcceptedEmptyWhenNoSubmission) {
    auto b = make_service();
    SubmissionListQuery q;
    q.page = 1; q.page_size = 10;
    auto r = b.service->list_public_accepted(q);
    EXPECT_EQ(r.total, 0);
    EXPECT_TRUE(r.items.empty());
}

}  // namespace
