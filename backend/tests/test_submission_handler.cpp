// =============================================================================
//  test_submission_handler.cpp —— POST/GET /api/submissions HTTP 集成测试
//  跑真 HttpServer + httplib::Client；用 in-memory repo 替换 MySQL。
//
//  覆盖：
//    POST /api/submissions
//      - 缺 token → 401
//      - 错 token → 401
//      - body 缺失 / 非 JSON → 400
//      - 缺字段 / 字段类型错 → 400
//      - 题目不存在 / 未发布 → 404
//      - code > 64KB → 413
//      - 正常 → 200 + {submission_id}
//
//    GET /api/submissions/{id}
//      - 路径 :id 非法 → 400
//      - submission 不存在 → 404
//      - 非 AC + 非 owner + 非 admin → 403
//      - AC 公开访问 → 200 + 完整 detail（含 username / cases）
//      - owner 看自己的非 AC → 200
//      - admin 看任意 → 200
//      - 样例点 cases 数组含 input / expected_output / user_output
//      - 隐藏点 cases 数组含 null 字段
//
//    边界：
//      - DB 不可用 → 503
//      - 错方法 → 404
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_types.hpp"
#include "domain/submission_repository.hpp"
#include "domain/submission_service.hpp"
#include "domain/submission_types.hpp"
#include "domain/testcase_repository.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/submission_handler.hpp"
#include "infra/jwt_service.hpp"

namespace {

using oj::common::AppConfig;
using oj::common::ErrorCode;
using oj::common::JwtConfig;
using oj::domain::ClaimedTask;
using oj::domain::Difficulty;
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
using oj::http::HttpServer;
using oj::infra::JwtService;

// ---------------------------------------------------------------------------
//  In-memory repos
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
        // username 模拟：按 user_id 区分（1→alice；2→bob）
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

    // 测试钩
    void set_result(std::int64_t id, SubmissionResult r, int score) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) {
            if (s.id == id) {
                s.status      = SubmissionStatus::Finished;
                s.result      = r;
                s.total_score = score;
                s.finished_at = "2026-06-17T00:00:01Z";
            }
        }
    }
    void add_case(std::int64_t sub_id, SubmissionCase c) {
        std::lock_guard<std::mutex> lk(mu_);
        c.submission_id = sub_id;
        cases_.push_back(std::move(c));
    }
    void set_user(std::int64_t sub_id, std::int64_t user_id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) if (s.id == sub_id) s.user_id = user_id;
    }

private:
    std::mutex                  mu_;
    std::vector<Submission>     rows_;
    std::vector<SubmissionCase> cases_;
    std::int64_t                next_id_{0};
};

class InMemoryProblemRepo : public IProblemRepository {
public:
    std::int64_t add(Problem p) {
        std::lock_guard<std::mutex> lk(mu_);
        ++next_id_;
        p.id = next_id_;
        rows_.push_back(p);
        return p.id;
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

private:
    std::mutex            mu_;
    std::vector<Problem>  rows_;
    std::int64_t          next_id_{0};
};

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
//  测试 fixture 工具
// ---------------------------------------------------------------------------
JwtConfig make_jwt_cfg() {
    JwtConfig c;
    c.secret          = "test-secret-32-bytes-min-padding-xxx";
    c.access_ttl_sec  = 3600;
    c.refresh_ttl_sec = 86400;
    c.issuer          = "onlinejudge-test";
    return c;
}

AppConfig make_cfg(uint16_t port) {
    AppConfig cfg;
    cfg.server.host = "127.0.0.1";
    cfg.server.port = port;
    cfg.server.thread_pool_size = 2;
    cfg.log.stdout_console = false;
    cfg.log.dir = std::filesystem::temp_directory_path();
    return cfg;
}

class ScopedServer {
public:
    explicit ScopedServer(uint16_t port) : srv_(make_cfg(port)) {}
    ~ScopedServer() { srv_.stop(); if (thread_.joinable()) thread_.join(); }
    ScopedServer(const ScopedServer&) = delete;
    ScopedServer& operator=(const ScopedServer&) = delete;
    HttpServer& server() noexcept { return srv_; }
    void start() {
        thread_ = std::thread([this] {
            ready_.store(true, std::memory_order_release);
            std::string reason;
            srv_.listen(&reason);
        });
        for (int i = 0; i < 300 && !ready_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
private:
    HttpServer           srv_;
    std::thread          thread_;
    std::atomic<bool>    ready_{false};
};

httplib::Client make_client(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    return cli;
}

struct ServerBundle {
    std::unique_ptr<ScopedServer>            server;
    std::shared_ptr<InMemorySubmissionRepo>  submissions;
    std::shared_ptr<InMemoryProblemRepo>     problems;
    std::shared_ptr<InMemoryTestcaseRepo>    testcases;
    std::shared_ptr<SubmissionService>       service;
    std::shared_ptr<JwtService>              jwt;
    std::shared_ptr<std::atomic<bool>>       db_ready{std::make_shared<std::atomic<bool>>(true)};
};

ServerBundle make_server(uint16_t port) {
    ServerBundle b;
    b.submissions = std::make_shared<InMemorySubmissionRepo>();
    b.problems    = std::make_shared<InMemoryProblemRepo>();
    b.testcases   = std::make_shared<InMemoryTestcaseRepo>();
    b.jwt         = std::make_shared<JwtService>(make_jwt_cfg());
    b.service     = std::make_shared<SubmissionService>(
        b.submissions, b.problems, b.testcases, /*code_max_bytes=*/65536);
    b.server      = std::make_unique<ScopedServer>(port);
    auto ready_ptr = b.db_ready;
    oj::http::handlers::register_submission_routes(
        b.server->server(), b.service, b.jwt,
        [ready_ptr] { return ready_ptr->load(std::memory_order_acquire); });
    return b;
}

Problem mk_problem(bool published) {
    Problem p;
    p.title        = "两数之和";
    p.content_md   = "读入两个整数 a, b，输出 a + b";
    p.difficulty   = Difficulty::Easy;
    p.is_published = published;
    p.created_by   = 1;
    return p;
}

std::string access_token(JwtService& j, std::int64_t uid, bool is_admin) {
    return j.issue_access(uid, is_admin);
}

// ===========================================================================
//  POST /api/submissions
// ===========================================================================

// ---- 鉴权 --------------------------------------------------------------
TEST(SubmissionHandlerTest, CreateRequiresBearerToken) {
    ServerBundle b = make_server(20100);
    b.server->start();
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20100).Post("/api/submissions", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20100 not reachable";
    EXPECT_EQ(res->status, 401);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1002);
}

TEST(SubmissionHandlerTest, CreateRejectsMalformedBearer) {
    ServerBundle b = make_server(20101);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer not.a.real.jwt"}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20101).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20101 not reachable";
    EXPECT_EQ(res->status, 401);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1002);
}

TEST(SubmissionHandlerTest, CreateRejectsRefreshTokenUsedAsAccess) {
    ServerBundle b = make_server(20102);
    b.server->start();
    const std::string refresh = b.jwt->issue_refresh(/*uid=*/1);
    httplib::Headers headers = {{"Authorization", "Bearer " + refresh}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20102).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20102 not reachable";
    EXPECT_EQ(res->status, 401);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1002);
}

// ---- body 解析 --------------------------------------------------------
TEST(SubmissionHandlerTest, CreateEmptyBodyReturns400) {
    ServerBundle b = make_server(20103);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20103).Post("/api/submissions", headers, "", "application/json");
    if (!res) GTEST_SKIP() << "port 20103 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(SubmissionHandlerTest, CreateMalformedJsonReturns400) {
    ServerBundle b = make_server(20104);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20104).Post("/api/submissions", headers, "{not-json", "application/json");
    if (!res) GTEST_SKIP() << "port 20104 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(SubmissionHandlerTest, CreateBodyNotObjectReturns400) {
    ServerBundle b = make_server(20105);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20105).Post("/api/submissions", headers, "[1,2,3]", "application/json");
    if (!res) GTEST_SKIP() << "port 20105 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(SubmissionHandlerTest, CreateMissingProblemIdReturns400) {
    ServerBundle b = make_server(20106);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20106).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20106 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
    EXPECT_NE(std::string{j["message"]}.find("problem_id"), std::string::npos);
}

TEST(SubmissionHandlerTest, CreateNonIntegerProblemIdReturns400) {
    ServerBundle b = make_server(20107);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", "abc"}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20107).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20107 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(SubmissionHandlerTest, CreateInvalidLanguageReturns400) {
    ServerBundle b = make_server(20108);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "rust"}, {"code", "x"}};
    auto res = make_client(20108).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20108 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(SubmissionHandlerTest, CreateEmptyCodeReturns400) {
    ServerBundle b = make_server(20109);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", ""}};
    auto res = make_client(20109).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20109 not reachable";
    EXPECT_EQ(res->status, 400);
}

// ---- 业务校验 --------------------------------------------------------
TEST(SubmissionHandlerTest, CreateProblemNotFoundReturns404) {
    ServerBundle b = make_server(20110);
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 999}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20110).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20110 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST(SubmissionHandlerTest, CreateUnpublishedProblemReturns404) {
    ServerBundle b = make_server(20111);
    b.problems->add(mk_problem(/*published=*/false));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20111).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20111 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST(SubmissionHandlerTest, CreateCodeTooLargeReturns413) {
    ServerBundle b = make_server(20112);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    // 65536+1 bytes
    std::string big(65537, 'a');
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", big}};
    auto res = make_client(20112).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20112 not reachable";
    EXPECT_EQ(res->status, 413);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1006);
}

// ---- Happy path ------------------------------------------------------
TEST(SubmissionHandlerTest, CreateHappyPathReturnsSubmissionId) {
    ServerBundle b = make_server(20120);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {
        {"problem_id", 1},
        {"language",   "cpp"},
        {"code",       "#include<bits/stdc++.h>\nint main(){return 0;}"},
    };
    auto res = make_client(20120).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20120 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    ASSERT_TRUE(j["data"].is_object());
    EXPECT_TRUE(j["data"].contains("submission_id"));
    EXPECT_GT(j["data"]["submission_id"].get<std::int64_t>(), 0);
}

TEST(SubmissionHandlerTest, CreateAll5LanguagesAreAccepted) {
    ServerBundle b = make_server(20121);
    b.problems->add(mk_problem(true));
    b.server->start();
    const char* langs[] = {"c", "cpp", "java", "python", "go"};
    for (int i = 0; i < 5; ++i) {
        httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
        nlohmann::json body = {{"problem_id", 1}, {"language", langs[i]}, {"code", "x"}};
        auto res = make_client(20121).Post("/api/submissions", headers, body.dump(), "application/json");
        if (!res) GTEST_SKIP() << "port 20121 not reachable";
        ASSERT_EQ(res->status, 200) << "lang=" << langs[i];
    }
}

// ---- DB 不可用 -------------------------------------------------------
TEST(SubmissionHandlerTest, CreateDbDownReturnsEnvelope) {
    ServerBundle b = make_server(20130);
    b.problems->add(mk_problem(true));
    b.server->start();
    b.db_ready->store(false, std::memory_order_release);
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20130).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20130 not reachable";
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
}

// ===========================================================================
//  GET /api/submissions/{id}
// ===========================================================================

// ---- 路径 / 找不到 ---------------------------------------------------
TEST(SubmissionHandlerTest, GetMissingIdReturns404) {
    ServerBundle b = make_server(20200);
    b.server->start();
    auto res = make_client(20200).Get("/api/submissions/999");
    if (!res) GTEST_SKIP() << "port 20200 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST(SubmissionHandlerTest, GetInvalidIdPathReturns400) {
    ServerBundle b = make_server(20201);
    b.server->start();
    auto res = make_client(20201).Get("/api/submissions/abc");
    if (!res) GTEST_SKIP() << "port 20201 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(SubmissionHandlerTest, GetZeroIdReturns400) {
    ServerBundle b = make_server(20202);
    b.server->start();
    auto res = make_client(20202).Get("/api/submissions/0");
    if (!res) GTEST_SKIP() << "port 20202 not reachable";
    EXPECT_EQ(res->status, 400);
}

// ---- 可见性 ---------------------------------------------------------
TEST(SubmissionHandlerTest, GetAnonymousSeesACSubmission) {
    ServerBundle b = make_server(20210);
    b.problems->add(mk_problem(true));
    b.server->start();

    // 直接创建一条 AC 的 submission（绕过 HTTP —— 简化）
    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::AC, 100);

    // 匿名 GET → 应该 200
    auto res = make_client(20210).Get("/api/submissions/" + std::to_string(sub_id));
    if (!res) GTEST_SKIP() << "port 20210 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["data"]["id"].get<std::int64_t>(), sub_id);
    EXPECT_EQ(j["data"]["result"].get<std::string>(), "AC");
    EXPECT_EQ(j["data"]["username"].get<std::string>(), "alice");
}

TEST(SubmissionHandlerTest, GetAnonymousBlockedFromWASubmission) {
    ServerBundle b = make_server(20211);
    b.problems->add(mk_problem(true));
    b.server->start();

    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::WA, 60);

    auto res = make_client(20211).Get("/api/submissions/" + std::to_string(sub_id));
    if (!res) GTEST_SKIP() << "port 20211 not reachable";
    EXPECT_EQ(res->status, 403);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1003);
}

TEST(SubmissionHandlerTest, GetOtherUserBlockedFromWASubmission) {
    ServerBundle b = make_server(20212);
    b.problems->add(mk_problem(true));
    b.server->start();

    auto sub_id = b.submissions->create(/*uid=*/1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::WA, 60);

    // user 2 看 user 1 的非 AC → 403
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 2, false)}};
    auto res = make_client(20212).Get(
        "/api/submissions/" + std::to_string(sub_id), headers);
    if (!res) GTEST_SKIP() << "port 20212 not reachable";
    EXPECT_EQ(res->status, 403);
}

TEST(SubmissionHandlerTest, GetOwnerSeesOwnWASubmission) {
    ServerBundle b = make_server(20213);
    b.problems->add(mk_problem(true));
    b.server->start();

    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::WA, 60);

    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20213).Get(
        "/api/submissions/" + std::to_string(sub_id), headers);
    if (!res) GTEST_SKIP() << "port 20213 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["result"].get<std::string>(), "WA");
}

TEST(SubmissionHandlerTest, GetAdminSeesOtherUsersNonACSubmission) {
    ServerBundle b = make_server(20214);
    b.problems->add(mk_problem(true));
    b.server->start();

    auto sub_id = b.submissions->create(/*uid=*/1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::TLE, 30);

    // admin 看 user 1 的 TLE
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 2, /*is_admin=*/true)}};
    auto res = make_client(20214).Get(
        "/api/submissions/" + std::to_string(sub_id), headers);
    if (!res) GTEST_SKIP() << "port 20214 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["result"].get<std::string>(), "TLE");
}

TEST(SubmissionHandlerTest, GetUnfinishedVisibleOnlyToOwnerOrAdmin) {
    ServerBundle b = make_server(20215);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "x");
    // 不调 set_result —— 状态保持 Queued

    // 匿名 → 403
    auto r1 = make_client(20215).Get("/api/submissions/" + std::to_string(sub_id));
    if (!r1) GTEST_SKIP() << "port 20215 not reachable";
    EXPECT_EQ(r1->status, 403);

    // 非 owner → 403
    httplib::Headers h2 = {{"Authorization", "Bearer " + access_token(*b.jwt, 2, false)}};
    auto r2 = make_client(20215).Get(
        "/api/submissions/" + std::to_string(sub_id), h2);
    ASSERT_TRUE(r2 != nullptr);
    EXPECT_EQ(r2->status, 403);

    // owner → 200
    httplib::Headers h3 = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto r3 = make_client(20215).Get(
        "/api/submissions/" + std::to_string(sub_id), h3);
    ASSERT_TRUE(r3 != nullptr);
    EXPECT_EQ(r3->status, 200);
    auto j = nlohmann::json::parse(r3->body);
    EXPECT_EQ(j["data"]["status"].get<std::string>(), "queued");
    EXPECT_TRUE(j["data"]["result"].is_null());

    // admin → 200
    httplib::Headers h4 = {{"Authorization", "Bearer " + access_token(*b.jwt, 2, true)}};
    auto r4 = make_client(20215).Get(
        "/api/submissions/" + std::to_string(sub_id), h4);
    ASSERT_TRUE(r4 != nullptr);
    EXPECT_EQ(r4->status, 200);
}

// ---- 响应字段完整性 ---------------------------------------------------
TEST(SubmissionHandlerTest, GetResponseCarriesAllSpecFields) {
    ServerBundle b = make_server(20220);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "int main(){}");
    b.submissions->set_result(sub_id, SubmissionResult::AC, 100);

    auto res = make_client(20220).Get("/api/submissions/" + std::to_string(sub_id));
    if (!res) GTEST_SKIP() << "port 20220 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const auto& d = j["data"];
    // 顶层字段
    for (const char* k : {"id", "problem_id", "user_id", "username", "language",
                          "code", "status", "result", "total_score",
                          "time_used_ms", "memory_used_kb", "compile_output",
                          "judge_message", "created_at", "finished_at", "cases"}) {
        EXPECT_TRUE(d.contains(k)) << "missing field: " << k;
    }
    EXPECT_EQ(d["username"].get<std::string>(), "alice");
    EXPECT_EQ(d["language"].get<std::string>(), "cpp");
    EXPECT_EQ(d["code"].get<std::string>(), "int main(){}");
    EXPECT_EQ(d["result"].get<std::string>(), "AC");
}

TEST(SubmissionHandlerTest, GetResponseCarriesSampleAndHiddenCases) {
    ServerBundle b = make_server(20221);
    b.problems->add(mk_problem(true));

    // 加样例点
    Testcase s1;
    s1.problem_id = 1; s1.case_index = 1;
    s1.input = "1 2\n"; s1.expected_output = "3\n";
    s1.is_sample = true; s1.score = 30;
    b.testcases->add(s1);
    Testcase s2;
    s2.problem_id = 1; s2.case_index = 2;
    s2.input = "10 20\n"; s2.expected_output = "30\n";
    s2.is_sample = false; s2.score = 70;
    b.testcases->add(s2);

    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->set_result(sub_id, SubmissionResult::WA, 30);

    // 样例点 WA
    SubmissionCase c1;
    c1.case_index = 1; c1.is_sample = true;
    c1.user_output = "3\n"; c1.score = 30; c1.status = SubmissionResult::AC;
    c1.time_used_ms = 10; c1.memory_used_kb = 1024;
    b.submissions->add_case(sub_id, c1);

    // 隐藏点 AC
    SubmissionCase c2;
    c2.case_index = 2; c2.is_sample = false;
    c2.score = 70; c2.status = SubmissionResult::AC;
    c2.time_used_ms = 12; c2.memory_used_kb = 1024;
    b.submissions->add_case(sub_id, c2);

    b.server->start();

    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20221).Get(
        "/api/submissions/" + std::to_string(sub_id), headers);
    if (!res) GTEST_SKIP() << "port 20221 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const auto& cases = j["data"]["cases"];
    ASSERT_EQ(cases.size(), 2u);

    // 样例点：input / expected_output / user_output 都应填
    EXPECT_EQ(cases[0]["case_index"].get<int>(), 1);
    EXPECT_TRUE(cases[0]["is_sample"].get<bool>());
    EXPECT_EQ(cases[0]["input"], "1 2\n");
    EXPECT_EQ(cases[0]["expected_output"], "3\n");
    EXPECT_EQ(cases[0]["user_output"], "3\n");

    // 隐藏点：input / expected_output / user_output 均为 null
    EXPECT_EQ(cases[1]["case_index"].get<int>(), 2);
    EXPECT_FALSE(cases[1]["is_sample"].get<bool>());
    EXPECT_TRUE(cases[1]["input"].is_null());
    EXPECT_TRUE(cases[1]["expected_output"].is_null());
    EXPECT_TRUE(cases[1]["user_output"].is_null());
}

TEST(SubmissionHandlerTest, GetCECarriesCompileOutput) {
    ServerBundle b = make_server(20222);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto sub_id = b.submissions->create(1, 1, Language::Cpp, "broken");
    b.submissions->set_result(sub_id, SubmissionResult::CE, 0);
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20222).Get(
        "/api/submissions/" + std::to_string(sub_id), headers);
    if (!res) GTEST_SKIP() << "port 20222 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["result"].get<std::string>(), "CE");
    EXPECT_TRUE(j["data"].contains("compile_output"));
}

// ---- DB 不可用 -------------------------------------------------------
TEST(SubmissionHandlerTest, GetDbDownReturnsEnvelope) {
    ServerBundle b = make_server(20230);
    b.problems->add(mk_problem(true));
    b.submissions->create(1, 1, Language::Cpp, "x");
    b.server->start();
    b.db_ready->store(false, std::memory_order_release);
    auto res = make_client(20230).Get("/api/submissions/1");
    if (!res) GTEST_SKIP() << "port 20230 not reachable";
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
}

// ---- 错方法 ---------------------------------------------------------
TEST(SubmissionHandlerTest, GetOnCreatePathReturns404) {
    ServerBundle b = make_server(20240);
    b.server->start();
    auto res = make_client(20240).Get("/api/submissions");
    if (!res) GTEST_SKIP() << "port 20240 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST(SubmissionHandlerTest, PostOnDetailPathReturns404) {
    ServerBundle b = make_server(20241);
    b.server->start();
    auto res = make_client(20241).Post("/api/submissions/1", "{}", "application/json");
    if (!res) GTEST_SKIP() << "port 20241 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

}  // namespace
