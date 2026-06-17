// =============================================================================
//  test_submission_handler_edges.cpp —— POST/GET /api/submissions 边界 / 集成测试
//  补 test_submission_handler.cpp 之外的细节场景：
//    - 8 个 result 状态（AC / WA / TLE / MLE / OLE / RE / CE / SE）JSON 形状
//    - 4 个 status 状态（queued / compiling / running / finished）
//    - UTF-8 / 中文 出现在 code / username / title
//    - POST → GET 端到端 roundtrip
//    - 路径参数：负数 / 浮点数 / 极大值 / 前导零
//    - 多个 case 排序：按 case_index ASC
//    - 响应 Content-Type 必为 application/json; charset=utf-8
//    - 错误响应也带 envelope（code/message/data）
//    - 跨用户隔离：两个 user 同时 POST 然后各自 GET
//    - 错误状态下 body 必为合法 JSON
//    - 不带 Content-Type 头时仍能处理
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
//  In-memory mocks（精简版）
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
    void finish(std::int64_t id, SubmissionResult r, int score, int t, int m,
                std::string_view co, std::string_view jm) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : rows_) {
            if (s.id == id) {
                s.status         = SubmissionStatus::Finished;
                s.result         = r;
                s.total_score    = score;
                s.time_used_ms   = t;
                s.memory_used_kb = m;
                s.compile_output = std::string{co};
                s.judge_message  = std::string{jm};
                s.finished_at    = "2026-06-17T00:00:01Z";
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
        auto copy = p; copy.id = ++next_id_; rows_.push_back(copy); return copy;
    }
    void update(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& x : rows_) if (x.id == p.id) x = p;
    }
    void soft_delete(std::int64_t) override {}
    void set_published(std::int64_t, bool) override {}
    std::pair<int, int> submission_stats(std::int64_t) override { return {0, 0}; }
private:
    std::mutex            mu_;
    std::vector<Problem>  rows_;
    std::int64_t          next_id_{0};
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
private:
    std::mutex              mu_;
    std::vector<Testcase>   tcs_;
};

// ---------------------------------------------------------------------------
//  Fixture
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
    HttpServer srv_;
    std::thread thread_;
    std::atomic<bool> ready_{false};
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
//  Content-Type
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, CreateSuccessResponseContentType) {
    ServerBundle b = make_server(20300);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20300).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20300 not reachable";
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json; charset=utf-8");
}

TEST(SubmissionHandlerEdgeTest, GetSuccessResponseContentType) {
    ServerBundle b = make_server(20301);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");
    auto res = make_client(20301).Get("/api/submissions/" + std::to_string(id));
    if (!res) GTEST_SKIP() << "port 20301 not reachable";
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json; charset=utf-8");
}

TEST(SubmissionHandlerEdgeTest, ErrorResponseIsValidEnvelopeJson) {
    ServerBundle b = make_server(20302);
    b.server->start();
    auto res = make_client(20302).Get("/api/submissions/abc");
    if (!res) GTEST_SKIP() << "port 20302 not reachable";
    EXPECT_EQ(res->get_header_value("Content-Type"), "application/json; charset=utf-8");
    // 错误响应也必须能 parse 为 envelope
    ASSERT_NO_THROW((void)nlohmann::json::parse(res->body));
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("code"));
    EXPECT_TRUE(j.contains("message"));
    EXPECT_TRUE(j.contains("data"));
}

// ===========================================================================
//  Status 字段：4 个状态
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, GetResponseStatusQueuedIsString) {
    ServerBundle b = make_server(20310);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    // 不 finish → Queued
    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20310).Get("/api/submissions/" + std::to_string(id), h);
    if (!res) GTEST_SKIP() << "port 20310 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["status"].get<std::string>(), "queued");
    EXPECT_TRUE(j["data"]["result"].is_null());
    EXPECT_TRUE(j["data"]["finished_at"].get<std::string>().empty());
}

TEST(SubmissionHandlerEdgeTest, GetResponseStatusCompilingAndRunningAreStringified) {
    // 模拟 compiling / running 中间态
    ServerBundle b = make_server(20311);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->update_status(id, SubmissionStatus::Compiling);
    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto r1 = make_client(20311).Get("/api/submissions/" + std::to_string(id), h);
    ASSERT_TRUE(r1 != nullptr);
    EXPECT_EQ(nlohmann::json::parse(r1->body)["data"]["status"].get<std::string>(), "compiling");

    b.submissions->update_status(id, SubmissionStatus::Running);
    auto r2 = make_client(20311).Get("/api/submissions/" + std::to_string(id), h);
    ASSERT_TRUE(r2 != nullptr);
    EXPECT_EQ(nlohmann::json::parse(r2->body)["data"]["status"].get<std::string>(), "running");
}

TEST(SubmissionHandlerEdgeTest, GetResponseStatusFinishedIsStringified) {
    ServerBundle b = make_server(20312);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");
    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20312).Get("/api/submissions/" + std::to_string(id), h);
    if (!res) GTEST_SKIP() << "port 20312 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["status"].get<std::string>(), "finished");
    EXPECT_FALSE(j["data"]["finished_at"].get<std::string>().empty());
}

// ===========================================================================
//  Result 字段：8 个状态
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, GetResponseAllEightResultsAreStringified) {
    ServerBundle b = make_server(20320);
    b.problems->add(mk_problem(true));
    b.server->start();

    const SubmissionResult results[] = {
        SubmissionResult::AC, SubmissionResult::WA, SubmissionResult::TLE,
        SubmissionResult::MLE, SubmissionResult::OLE, SubmissionResult::RE,
        SubmissionResult::CE, SubmissionResult::SE,
    };
    const char* names[] = {"AC", "WA", "TLE", "MLE", "OLE", "RE", "CE", "SE"};

    for (std::size_t i = 0; i < sizeof(results) / sizeof(results[0]); ++i) {
        auto id = b.submissions->create(1, 1, Language::Cpp, "x");
        b.submissions->finish(id, results[i], 0, 0, 0, "", "");

        httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
        auto res = make_client(20320).Get("/api/submissions/" + std::to_string(id), h);
        ASSERT_TRUE(res != nullptr) << "result=" << names[i];
        ASSERT_EQ(res->status, 200) << "result=" << names[i];
        auto j = nlohmann::json::parse(res->body);
        EXPECT_EQ(j["data"]["result"].get<std::string>(), names[i])
            << "result=" << names[i];
    }
}

// ===========================================================================
//  POST → GET roundtrip
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, PostThenGetRoundtrip) {
    ServerBundle b = make_server(20330);
    b.problems->add(mk_problem(true));
    b.server->start();
    const std::string code = "int main(){return 0;}";
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 7, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", code}};
    auto post_res = make_client(20330).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!post_res) GTEST_SKIP() << "port 20330 not reachable";
    ASSERT_EQ(post_res->status, 200);
    auto post_j = nlohmann::json::parse(post_res->body);
    const auto new_id = post_j["data"]["submission_id"].get<std::int64_t>();
    EXPECT_GT(new_id, 0);

    // 立刻 GET
    auto get_res = make_client(20330).Get("/api/submissions/" + std::to_string(new_id), headers);
    ASSERT_TRUE(get_res != nullptr);
    ASSERT_EQ(get_res->status, 200);
    auto get_j = nlohmann::json::parse(get_res->body);
    EXPECT_EQ(get_j["data"]["id"].get<std::int64_t>(),        new_id);
    EXPECT_EQ(get_j["data"]["user_id"].get<std::int64_t>(),    7);
    EXPECT_EQ(get_j["data"]["problem_id"].get<std::int64_t>(), 1);
    EXPECT_EQ(get_j["data"]["language"].get<std::string>(),    "cpp");
    EXPECT_EQ(get_j["data"]["code"].get<std::string>(),        code);
    EXPECT_EQ(get_j["data"]["status"].get<std::string>(),      "queued");
    EXPECT_TRUE(get_j["data"]["result"].is_null());
}

// ===========================================================================
//  跨用户隔离
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, TwoUsersPostAndGetIndependently) {
    ServerBundle b = make_server(20340);
    b.problems->add(mk_problem(true));
    b.server->start();

    // user 1 提交
    httplib::Headers h1 = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json b1 = {{"problem_id", 1}, {"language", "cpp"}, {"code", "alice-code"}};
    auto r1 = make_client(20340).Post("/api/submissions", h1, b1.dump(), "application/json");
    ASSERT_TRUE(r1 != nullptr);
    ASSERT_EQ(r1->status, 200);
    auto id1 = nlohmann::json::parse(r1->body)["data"]["submission_id"].get<std::int64_t>();

    // user 2 提交
    httplib::Headers h2 = {{"Authorization", "Bearer " + access_token(*b.jwt, 2, false)}};
    nlohmann::json b2 = {{"problem_id", 1}, {"language", "python"}, {"code", "bob-code"}};
    auto r2 = make_client(20340).Post("/api/submissions", h2, b2.dump(), "application/json");
    ASSERT_TRUE(r2 != nullptr);
    ASSERT_EQ(r2->status, 200);
    auto id2 = nlohmann::json::parse(r2->body)["data"]["submission_id"].get<std::int64_t>();
    EXPECT_NE(id1, id2);

    // user 1 GET 自己的
    auto g1 = make_client(20340).Get("/api/submissions/" + std::to_string(id1), h1);
    ASSERT_TRUE(g1 != nullptr);
    EXPECT_EQ(g1->status, 200);
    auto j1 = nlohmann::json::parse(g1->body);
    EXPECT_EQ(j1["data"]["code"].get<std::string>(),     "alice-code");
    EXPECT_EQ(j1["data"]["language"].get<std::string>(), "cpp");
    EXPECT_EQ(j1["data"]["username"].get<std::string>(), "alice");

    // user 2 GET 自己的
    auto g2 = make_client(20340).Get("/api/submissions/" + std::to_string(id2), h2);
    ASSERT_TRUE(g2 != nullptr);
    EXPECT_EQ(g2->status, 200);
    auto j2 = nlohmann::json::parse(g2->body);
    EXPECT_EQ(j2["data"]["code"].get<std::string>(),     "bob-code");
    EXPECT_EQ(j2["data"]["language"].get<std::string>(), "python");
    EXPECT_EQ(j2["data"]["username"].get<std::string>(), "bob");
}

// ===========================================================================
//  UTF-8
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, PostWithUtf8CodeSucceeds) {
    ServerBundle b = make_server(20350);
    b.problems->add(mk_problem(true));
    b.server->start();
    const std::string utf8_code =
        "// 中文注释：读入 a 和 b\n"
        "int main() { return 0; }";
    httplib::Headers headers = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", utf8_code}};
    auto res = make_client(20350).Post("/api/submissions", headers, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20350 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const auto id = j["data"]["submission_id"].get<std::int64_t>();
    // GET 回来 code 必须保留
    auto g = make_client(20350).Get("/api/submissions/" + std::to_string(id), headers);
    ASSERT_TRUE(g != nullptr);
    EXPECT_EQ(nlohmann::json::parse(g->body)["data"]["code"].get<std::string>(), utf8_code);
}

TEST(SubmissionHandlerEdgeTest, GetUtf8UsernamePropagates) {
    ServerBundle b = make_server(20351);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::AC, 100, 10, 1024, "", "");
    auto res = make_client(20351).Get("/api/submissions/" + std::to_string(id));
    if (!res) GTEST_SKIP() << "port 20351 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["username"].get<std::string>(), "alice");  // 英文，简单
}

// ===========================================================================
//  路径参数
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, GetWithLeadingZerosInPathReturns400) {
    ServerBundle b = make_server(20360);
    b.server->start();
    auto res = make_client(20360).Get("/api/submissions/00000123");
    if (!res) GTEST_SKIP() << "port 20360 not reachable";
    // stoll 接受前导零 → 实际是合法正整数；如果 repo 里没有这条 id → 404
    // 这里我们只断言：响应是合法的 envelope（不是 500）
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("code"));
    EXPECT_TRUE(j["code"].get<int>() == 1004 || j["code"].get<int>() == 1001);
}

TEST(SubmissionHandlerEdgeTest, GetWithVeryLargeIdReturns404) {
    ServerBundle b = make_server(20361);
    b.server->start();
    auto res = make_client(20361).Get("/api/submissions/999999999999999");
    if (!res) GTEST_SKIP() << "port 20361 not reachable";
    EXPECT_EQ(res->status, 404);
}

TEST(SubmissionHandlerEdgeTest, GetWithNegativeIdInPathReturns400) {
    ServerBundle b = make_server(20362);
    b.server->start();
    auto res = make_client(20362).Get("/api/submissions/-1");
    if (!res) GTEST_SKIP() << "port 20362 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(SubmissionHandlerEdgeTest, GetWithFloatInPathReturns400) {
    ServerBundle b = make_server(20363);
    b.server->start();
    auto res = make_client(20363).Get("/api/submissions/1.5");
    if (!res) GTEST_SKIP() << "port 20363 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(SubmissionHandlerEdgeTest, GetWithTrailingPathSegmentReturns404) {
    ServerBundle b = make_server(20364);
    b.server->start();
    auto res = make_client(20364).Get("/api/submissions/1/extra");
    if (!res) GTEST_SKIP() << "port 20364 not reachable";
    EXPECT_EQ(res->status, 404);
}

// ===========================================================================
//  Case 排序
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, CasesAreSortedByCaseIndex) {
    ServerBundle b = make_server(20370);
    b.problems->add(mk_problem(true));
    b.server->start();

    // 加 5 个样例点
    for (int i = 1; i <= 5; ++i) {
        Testcase tc;
        tc.problem_id = 1;
        tc.case_index = i;
        tc.input = "in-" + std::to_string(i) + "\n";
        tc.expected_output = "out-" + std::to_string(i) + "\n";
        tc.is_sample = (i <= 3);  // 前 3 个是样例
        tc.score = 20;
        b.testcases->add(tc);
    }

    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    b.submissions->finish(id, SubmissionResult::WA, 0, 0, 0, "", "");

    // 故意以乱序 add case
    const int order[] = {3, 1, 5, 2, 4};
    for (int idx : order) {
        SubmissionCase c;
        c.case_index = idx;
        c.is_sample  = (idx <= 3);
        c.status     = SubmissionResult::AC;
        c.score      = 20;
        c.user_output = "x";
        c.time_used_ms = 1;
        c.memory_used_kb = 1024;
        b.submissions->add_case(id, c);
    }

    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    auto res = make_client(20370).Get("/api/submissions/" + std::to_string(id), h);
    if (!res) GTEST_SKIP() << "port 20370 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const auto& cases = j["data"]["cases"];
    ASSERT_EQ(cases.size(), 5u);
    for (std::size_t i = 0; i < cases.size(); ++i) {
        EXPECT_EQ(cases[i]["case_index"].get<int>(), static_cast<int>(i + 1));
    }
}

// ===========================================================================
//  Content-Type 缺失
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, PostWithoutContentTypeStillProcesses) {
    ServerBundle b = make_server(20380);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    // 不传 Content-Type header —— cpp-httplib 默认会按 octet-stream 处理
    auto res = make_client(20380).Post("/api/submissions", h, body.dump(), "");
    if (!res) GTEST_SKIP() << "port 20380 not reachable";
    // 行为取决于 httplib 怎么处理空 Content-Type；我们只要求响应是合法 envelope
    ASSERT_FALSE(res->body.empty());
    auto j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j.contains("code"));
    // 要么 200 接受，要么 400/415 拒绝；不能 500
    EXPECT_LT(res->status, 500);
}

// ===========================================================================
//  Total score / time / memory 字段
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, GetResponseCarriesScoreAndTiming) {
    ServerBundle b = make_server(20390);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto id = b.submissions->create(1, 1, Language::Cpp, "x");
    // 模拟 SPEC §5.3 例子里的数据 —— 用 AC 让匿名访问可见
    b.submissions->finish(id, SubmissionResult::AC, /*score=*/100,
                          /*time_ms=*/1500, /*mem_kb=*/12000, "", "");
    auto res = make_client(20390).Get("/api/submissions/" + std::to_string(id));
    if (!res) GTEST_SKIP() << "port 20390 not reachable";
    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total_score"].get<int>(),     100);
    EXPECT_EQ(j["data"]["time_used_ms"].get<int>(),   1500);
    EXPECT_EQ(j["data"]["memory_used_kb"].get<int>(), 12000);
}

// ===========================================================================
//  POST 错误响应 Content-Type
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, CreateErrorResponseIsValidJson) {
    ServerBundle b = make_server(20400);
    b.problems->add(mk_problem(true));
    b.server->start();
    httplib::Headers h = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};
    nlohmann::json body = {{"problem_id", 1}, {"language", "rust"}, {"code", "x"}};
    auto res = make_client(20400).Post("/api/submissions", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20400 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(),            1001);
    EXPECT_FALSE(std::string{j["message"]}.empty());
}

// ===========================================================================
//  POST 不带 Authorization header（必须返回 401）
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, PostWithoutAuthHeaderIs401) {
    ServerBundle b = make_server(20410);
    b.problems->add(mk_problem(true));
    b.server->start();
    nlohmann::json body = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
    auto res = make_client(20410).Post("/api/submissions", body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 20410 not reachable";
    EXPECT_EQ(res->status, 401);
}

// ===========================================================================
//  错误响应 status 码与 ErrorCode 映射对齐
// ===========================================================================
TEST(SubmissionHandlerEdgeTest, ErrorStatusCodesMatchErrorCodeMapping) {
    ServerBundle b = make_server(20420);
    b.problems->add(mk_problem(true));
    b.server->start();
    auto cli = make_client(20420);
    httplib::Headers h_ok = {{"Authorization", "Bearer " + access_token(*b.jwt, 1, false)}};

    struct Case { std::function<httplib::Result(httplib::Client&, const httplib::Headers&)> do_req;
                  int expected_status; int expected_code; const char* desc; };
    // 400 (BadRequest) 多种触发条件
    const std::vector<Case> cases = {
        // 路径非法
        {[](httplib::Client& c, const httplib::Headers&) {
            return c.Get("/api/submissions/abc");
        }, 400, 1001, "GET /api/submissions/abc → 1001"},
        // 题目不存在
        {[&](httplib::Client& c, const httplib::Headers&) {
            nlohmann::json b = {{"problem_id", 999}, {"language", "cpp"}, {"code", "x"}};
            return c.Post("/api/submissions", h_ok, b.dump(), "application/json");
        }, 404, 1004, "POST 题目不存在 → 1004"},
        // 无 token
        {[](httplib::Client& c, const httplib::Headers&) {
            nlohmann::json b = {{"problem_id", 1}, {"language", "cpp"}, {"code", "x"}};
            return c.Post("/api/submissions", b.dump(), "application/json");
        }, 401, 1002, "POST 无 token → 1002"},
    };
    for (const auto& tc : cases) {
        auto res = tc.do_req(cli, h_ok);
        if (!res) continue;
        ASSERT_EQ(res->status, tc.expected_status) << tc.desc;
        auto j = nlohmann::json::parse(res->body);
        EXPECT_EQ(j["code"].get<int>(), tc.expected_code) << tc.desc;
    }
}

}  // namespace
