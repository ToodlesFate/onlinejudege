// =============================================================================
//  test_admin_problem_handler.cpp —— 后台题目录入 / 编辑 / 上下架 HTTP 测试
//  跑真 HttpServer + httplib::Client，注入 InMemoryXxxRepo + 真实 JwtService
//
//  覆盖：
//    1) 鉴权：缺 token / 错 token / 普通用户 → 1002/1003
//    2) GET  /api/admin/problems              列表（admin 视角全可见）
//    3) GET  /api/admin/problems/:id/edit-data 含全部 testcases
//    4) POST /api/admin/problems              创建 + 字段校验
//    5) PUT  /api/admin/problems/:id          全量更新
//    6) DEL  /api/admin/problems/:id          软删（is_published=0）
//    7) PATCH /api/admin/problems/:id/publish 上下架
//    8) DB 不可用 → 1008
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"
#include "domain/tag_repository.hpp"
#include "domain/testcase_repository.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/admin_problem_handler.hpp"
#include "infra/jwt_service.hpp"

namespace {

using oj::common::AppConfig;
using oj::common::JwtConfig;
using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::ITagRepository;
using oj::domain::ITestcaseRepository;
using oj::domain::Problem;
using oj::domain::ProblemService;
using oj::domain::Tag;
using oj::domain::Testcase;
using oj::http::HttpServer;
using oj::infra::JwtService;

// ---------------------------------------------------------------------------
//  In-memory mocks —— 支持 create/update/soft_delete/set_published +
//                              list_by_problem/replace_by_problem/create_many
//  testcases / tags 的可观察钩子让测试能验证"被实际写入"。
// ---------------------------------------------------------------------------
class InMemoryProblemRepo : public IProblemRepository {
public:
    std::optional<Problem> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& p : rows_) if (p.id == id) return p;
        return std::nullopt;
    }
    oj::domain::ProblemListResult list(const oj::domain::ProblemListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        oj::domain::ProblemListResult out;
        out.page = q.page;
        out.page_size = q.page_size;
        for (const auto& p : rows_) {
            if (!q.include_unpublished && !p.is_published) continue;
            out.items.push_back(make_item(p));
        }
        out.total = static_cast<std::int64_t>(out.items.size());
        return out;
    }
    Problem create(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
        Problem copy = p;
        copy.id = ++next_id_;
        // 模拟 DB created_at 行为
        copy.created_at = "2026-06-17T00:00:00Z";
        rows_.push_back(copy);
        ++create_count;
        return copy;
    }
    void update(const Problem& p) override {
        std::lock_guard<std::mutex> lk(mu_);
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
    std::int64_t seed(Problem p) {
        std::lock_guard<std::mutex> lk(mu_);
        p.id = ++next_id_;
        if (p.created_at.empty()) p.created_at = "2026-06-17T00:00:00Z";
        rows_.push_back(p);
        return p.id;
    }
    int  create_count        = 0;
    int  update_count        = 0;
    int  soft_delete_count   = 0;
    int  set_published_count = 0;
    std::vector<Problem> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return rows_;
    }
private:
    std::mutex            mu_;
    std::vector<Problem>  rows_;
    std::int64_t          next_id_{0};

    oj::domain::ProblemListItem make_item(const Problem& p) {
        oj::domain::ProblemListItem it;
        it.id           = p.id;
        it.title        = p.title;
        it.difficulty   = p.difficulty;
        it.is_published = p.is_published;
        it.created_by   = p.created_by;
        it.created_at   = p.created_at;
        return it;
    }
};

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
        for (auto c : cases) {
            c.problem_id = pid;
            rows_.push_back(c);
        }
        ++create_many_count;
    }
    void replace_by_problem(std::int64_t pid, const std::vector<Testcase>& cases) override {
        std::lock_guard<std::mutex> lk(mu_);
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
                                     [pid](const Testcase& t) { return t.problem_id == pid; }),
                    rows_.end());
        for (auto c : cases) {
            c.problem_id = pid;
            rows_.push_back(c);
        }
        ++replace_count;
    }
    void delete_by_problem(std::int64_t) override {}
    int create_many_count = 0;
    int replace_count     = 0;
    std::vector<Testcase> snapshot() {
        std::lock_guard<std::mutex> lk(mu_);
        return rows_;
    }
private:
    std::mutex              mu_;
    std::vector<Testcase>   rows_;
};

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
        pt_.erase(pid);
        for (int id : tag_ids) pt_.emplace(pid, id);
        ++set_count;
    }
    void seed_tag(Tag t) {
        std::lock_guard<std::mutex> lk(mu_);
        tags_[t.id] = t;
    }
    int set_count = 0;
private:
    std::mutex                                    mu_;
    std::map<int, Tag>                            tags_;
    std::multimap<std::int64_t, int>              pt_;
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
    HttpServer        srv_;
    std::thread       thread_;
    std::atomic<bool> ready_{false};
};

httplib::Client make_client(uint16_t port) {
    httplib::Client cli("127.0.0.1", port);
    cli.set_connection_timeout(2, 0);
    cli.set_read_timeout(2, 0);
    return cli;
}

struct ServerBundle {
    std::unique_ptr<ScopedServer>                server;
    std::shared_ptr<InMemoryProblemRepo>         problems;
    std::shared_ptr<InMemoryTestcaseRepo>        testcases;
    std::shared_ptr<InMemoryTagRepo>             tags;
    std::shared_ptr<ProblemService>              service;
    std::shared_ptr<JwtService>                  jwt;
    std::shared_ptr<std::atomic<bool>>           db_ready{std::make_shared<std::atomic<bool>>(true)};
};

ServerBundle make_server(uint16_t port) {
    ServerBundle b;
    b.problems  = std::make_shared<InMemoryProblemRepo>();
    b.testcases = std::make_shared<InMemoryTestcaseRepo>();
    b.tags      = std::make_shared<InMemoryTagRepo>();
    b.jwt       = std::make_shared<JwtService>(make_jwt_cfg());
    b.service   = std::make_shared<ProblemService>(b.problems, b.testcases, b.tags);
    b.server    = std::make_unique<ScopedServer>(port);
    auto ready_ptr = b.db_ready;
    oj::http::handlers::register_admin_problem_routes(
        b.server->server(), b.service, b.jwt,
        [ready_ptr] { return ready_ptr->load(std::memory_order_acquire); });
    return b;
}

std::string access_token(JwtService& j, std::int64_t uid, bool is_admin) {
    return j.issue_access(uid, is_admin);
}

nlohmann::json valid_create_body() {
    return nlohmann::json{
        {"title",           "两数之和"},
        {"content_md",      "读入两个整数 a, b；输出 a + b。\n样例：1 2 → 3"},
        {"difficulty",      "easy"},
        {"time_limit_ms",   2000},
        {"memory_limit_mb", 256},
        {"output_limit_mb", 64},
        {"is_published",    true},
        {"tag_ids",         nlohmann::json::array({1})},
        {"cases", nlohmann::json::array({
            {{"case_index", 1}, {"input", "1 2"}, {"expected_output", "3"},
             {"is_sample", true},  {"score", 60}},
            {{"case_index", 2}, {"input", "10 20"}, {"expected_output", "30"},
             {"is_sample", true},  {"score", 40}},
        })},
    };
}

// ===========================================================================
//  鉴权（所有 admin 路由都要求 admin 角色）
// ===========================================================================
TEST(AdminProblemHandlerAuthTest, AllRoutesRequireBearer) {
    ServerBundle b = make_server(22000);
    b.server->start();
    auto cli = make_client(22000);

    // GET 列表
    if (auto res = cli.Get("/api/admin/problems")) {
        EXPECT_EQ(res->status, 401);
        auto j = nlohmann::json::parse(res->body);
        EXPECT_EQ(j["code"].get<int>(), 1002);
    } else GTEST_SKIP() << "port 22000 not reachable";

    // POST 创建
    nlohmann::json body = valid_create_body();
    if (auto res = cli.Post("/api/admin/problems", body.dump(), "application/json")) {
        EXPECT_EQ(res->status, 401);
    }
    // PUT 更新
    if (auto res = cli.Put("/api/admin/problems/1", body.dump(), "application/json")) {
        EXPECT_EQ(res->status, 401);
    }
    // DELETE
    if (auto res = cli.Delete("/api/admin/problems/1")) {
        EXPECT_EQ(res->status, 401);
    }
    // PATCH publish
    if (auto res = cli.Patch("/api/admin/problems/1/publish",
                              R"({"is_published":true})", "application/json")) {
        EXPECT_EQ(res->status, 401);
    }
    // edit-data
    if (auto res = cli.Get("/api/admin/problems/1/edit-data")) {
        EXPECT_EQ(res->status, 401);
    }
}

TEST(AdminProblemHandlerAuthTest, AllRoutesRejectNonAdminToken) {
    ServerBundle b = make_server(22001);
    b.server->start();
    auto cli = make_client(22001);
    const std::string token = access_token(*b.jwt, /*uid=*/7, /*is_admin=*/false);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    nlohmann::json body = valid_create_body();

    if (auto res = cli.Get("/api/admin/problems", h)) {
        EXPECT_EQ(res->status, 403);
        auto j = nlohmann::json::parse(res->body);
        EXPECT_EQ(j["code"].get<int>(), 1003);
    }
    if (auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json")) {
        EXPECT_EQ(res->status, 403);
    }
    if (auto res = cli.Put("/api/admin/problems/1", h, body.dump(), "application/json")) {
        EXPECT_EQ(res->status, 403);
    }
    if (auto res = cli.Delete("/api/admin/problems/1", h)) {
        EXPECT_EQ(res->status, 403);
    }
    if (auto res = cli.Patch("/api/admin/problems/1/publish", h,
                              R"({"is_published":true})", "application/json")) {
        EXPECT_EQ(res->status, 403);
    }
    if (auto res = cli.Get("/api/admin/problems/1/edit-data", h)) {
        EXPECT_EQ(res->status, 403);
    }
}

TEST(AdminProblemHandlerAuthTest, AdminTokenIsAccepted) {
    ServerBundle b = make_server(22002);
    // 预置 1 条已发布题 + 1 条草稿（验证 list 包含未发布）
    Problem pub; pub.title = "已发布"; pub.difficulty = Difficulty::Easy;
    pub.is_published = true; pub.content_md = "x";
    Problem drf; drf.title = "草稿"; drf.difficulty = Difficulty::Easy;
    drf.is_published = false; drf.content_md = "x";
    b.problems->seed(pub);
    b.problems->seed(drf);
    b.server->start();
    auto cli = make_client(22002);
    const std::string token = access_token(*b.jwt, /*uid=*/1, /*is_admin=*/true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    auto res = cli.Get("/api/admin/problems", h);
    if (!res) GTEST_SKIP() << "port 22002 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["data"]["total"].get<int>(), 2);
}

// ===========================================================================
//  GET /api/admin/problems
// ===========================================================================
TEST(AdminListHandlerTest, ReturnsBothPublishedAndDraft) {
    ServerBundle b = make_server(22100);
    for (int i = 0; i < 5; ++i) {
        Problem p; p.title = "pub" + std::to_string(i); p.difficulty = Difficulty::Easy;
        p.is_published = true; p.content_md = "x";
        b.problems->seed(p);
    }
    for (int i = 0; i < 3; ++i) {
        Problem p; p.title = "drf" + std::to_string(i); p.difficulty = Difficulty::Easy;
        p.is_published = false; p.content_md = "x";
        b.problems->seed(p);
    }
    b.server->start();
    auto cli = make_client(22100);
    const std::string token = access_token(*b.jwt, 1, true);
    auto res = cli.Get("/api/admin/problems", {{"Authorization", "Bearer " + token}});
    if (!res) GTEST_SKIP() << "port 22100 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 8);
    EXPECT_EQ(j["data"]["items"].size(), 8u);
}

TEST(AdminListHandlerTest, EnvelopeShapeMatchesPublicList) {
    ServerBundle b = make_server(22101);
    Problem p; p.title = "x"; p.difficulty = Difficulty::Hard; p.is_published = true;
    p.content_md = "x"; p.created_by = 42;
    b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22101);
    const std::string token = access_token(*b.jwt, 1, true);
    auto res = cli.Get("/api/admin/problems", {{"Authorization", "Bearer " + token}});
    if (!res) GTEST_SKIP() << "port 22101 not reachable";
    auto j = nlohmann::json::parse(res->body);
    const auto& it = j["data"]["items"][0];
    EXPECT_TRUE(it.contains("id"));
    EXPECT_TRUE(it.contains("title"));
    EXPECT_TRUE(it.contains("difficulty"));
    EXPECT_TRUE(it.contains("is_published"));
    EXPECT_TRUE(it.contains("created_by"));
    EXPECT_TRUE(it.contains("created_at"));
    EXPECT_TRUE(it.contains("tags"));
    EXPECT_TRUE(it.contains("stats"));
    EXPECT_EQ(it["difficulty"].get<std::string>(), "hard");
    EXPECT_EQ(it["created_by"].get<int>(), 42);
}

// ===========================================================================
//  GET /api/admin/problems/:id/edit-data
// ===========================================================================
TEST(AdminEditDataHandlerTest, ReturnsFullDetailWithAllTestcases) {
    ServerBundle b = make_server(22200);
    Problem p; p.title = "两数之和"; p.difficulty = Difficulty::Easy;
    p.is_published = true; p.content_md = "x";
    p.time_limit_ms = 1500; p.memory_limit_mb = 128; p.output_limit_mb = 32;
    const std::int64_t pid = b.problems->seed(p);
    // 灌 2 个 tag
    b.tags->seed_tag({1, "数组", "数组"});
    b.tags->seed_tag({7, "动态规划", "dp"});
    b.tags->set_problem_tags(pid, {1, 7});
    // 灌 4 个 testcase：2 sample + 2 hidden
    for (int i = 1; i <= 4; ++i) {
        Testcase t; t.case_index = i;
        t.input = "in" + std::to_string(i);
        t.expected_output = "out" + std::to_string(i);
        t.is_sample = (i <= 2);
        t.score = 25;
        b.testcases->create_many(pid, {t});
    }
    b.server->start();
    auto cli = make_client(22200);
    const std::string token = access_token(*b.jwt, 1, true);
    auto res = cli.Get("/api/admin/problems/" + std::to_string(pid) + "/edit-data",
                       {{"Authorization", "Bearer " + token}});
    if (!res) GTEST_SKIP() << "port 22200 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const auto& data = j["data"];
    EXPECT_EQ(data["title"].get<std::string>(), "两数之和");
    EXPECT_EQ(data["time_limit_ms"].get<int>(), 1500);
    EXPECT_EQ(data["memory_limit_mb"].get<int>(), 128);
    EXPECT_EQ(data["output_limit_mb"].get<int>(), 32);
    EXPECT_EQ(data["tags"].size(), 2u);
    EXPECT_EQ(data["cases"].size(), 4u);
    // cases 包含 hidden（is_sample=false），公开 API 不返回
    int hidden = 0;
    for (const auto& c : data["cases"]) if (!c["is_sample"].get<bool>()) ++hidden;
    EXPECT_EQ(hidden, 2);
}

TEST(AdminEditDataHandlerTest, NotFoundReturns1004) {
    ServerBundle b = make_server(22201);
    b.server->start();
    auto cli = make_client(22201);
    const std::string token = access_token(*b.jwt, 1, true);
    auto res = cli.Get("/api/admin/problems/9999/edit-data",
                       {{"Authorization", "Bearer " + token}});
    if (!res) GTEST_SKIP() << "port 22201 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

// ===========================================================================
//  POST /api/admin/problems
// ===========================================================================
TEST(AdminCreateHandlerTest, CreatesAndPersists) {
    ServerBundle b = make_server(22300);
    b.tags->seed_tag({1, "数组", "数组"});
    b.tags->seed_tag({7, "动态规划", "dp"});
    b.server->start();
    auto cli = make_client(22300);
    const std::string token = access_token(*b.jwt, /*uid=*/1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token},
                          {"Content-Type",  "application/json"}};
    nlohmann::json body = valid_create_body();
    body["tag_ids"] = nlohmann::json::array({1, 7});

    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22300 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["data"]["id"].get<int>(), 1);
    EXPECT_EQ(j["data"]["title"].get<std::string>(), "两数之和");
    EXPECT_EQ(j["data"]["is_published"].get<bool>(), true);
    EXPECT_EQ(j["data"]["difficulty"].get<std::string>(), "easy");
    EXPECT_GT(j["data"]["created_at"].get<std::string>().size(), 0u);

    // 验证 repo 状态
    EXPECT_EQ(b.problems->create_count, 1);
    EXPECT_EQ(b.testcases->create_many_count, 1);
    EXPECT_EQ(b.tags->set_count, 1);
    auto all = b.problems->snapshot();
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].title, "两数之和");
    EXPECT_EQ(all[0].created_by, 1);  // admin uid
    auto tcs = b.testcases->snapshot();
    ASSERT_EQ(tcs.size(), 2u);
    EXPECT_EQ(tcs[0].score + tcs[1].score, 100);
    auto tag_ids = b.tags->tag_ids_of_problem(1);
    EXPECT_EQ(tag_ids.size(), 2u);
}

TEST(AdminCreateHandlerTest, EmptyBodyRejected) {
    ServerBundle b = make_server(22301);
    b.server->start();
    auto cli = make_client(22301);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token},
                          {"Content-Type",  "application/json"}};
    auto res = cli.Post("/api/admin/problems", h, "", "application/json");
    if (!res) GTEST_SKIP() << "port 22301 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(AdminCreateHandlerTest, InvalidJsonRejected) {
    ServerBundle b = make_server(22302);
    b.server->start();
    auto cli = make_client(22302);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token},
                          {"Content-Type",  "application/json"}};
    auto res = cli.Post("/api/admin/problems", h, "not-json", "application/json");
    if (!res) GTEST_SKIP() << "port 22302 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(AdminCreateHandlerTest, MissingTitleRejected) {
    ServerBundle b = make_server(22303);
    b.server->start();
    auto cli = make_client(22303);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body.erase("title");
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22303 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
    EXPECT_NE(j["message"].get<std::string>().find("title"), std::string::npos);
}

TEST(AdminCreateHandlerTest, TitleTooLongRejected) {
    ServerBundle b = make_server(22304);
    b.server->start();
    auto cli = make_client(22304);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["title"] = std::string(200, 'a');  // > 100
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22304 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(AdminCreateHandlerTest, ContentMdTooLargeRejected) {
    ServerBundle b = make_server(22305);
    b.server->start();
    auto cli = make_client(22305);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["content_md"] = std::string(70000, 'x');  // > 64KB
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22305 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_NE(j["message"].get<std::string>().find("content_md"), std::string::npos);
}

TEST(AdminCreateHandlerTest, InvalidDifficultyRejected) {
    ServerBundle b = make_server(22306);
    b.server->start();
    auto cli = make_client(22306);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["difficulty"] = "EASY";  // case sensitive
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22306 not reachable";
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(b.problems->create_count, 0);  // 没真正写库
}

TEST(AdminCreateHandlerTest, OutOfRangeLimitsRejected) {
    ServerBundle b = make_server(22307);
    b.server->start();
    auto cli = make_client(22307);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    for (auto&& [field, bad_value, hint] : std::vector<std::tuple<std::string, int, std::string>>{
        {"time_limit_ms",   0,         "time_limit_ms"},
        {"time_limit_ms",   99999,     "time_limit_ms"},
        {"memory_limit_mb", 32,        "memory_limit_mb"},
        {"memory_limit_mb", 9999,      "memory_limit_mb"},
        {"output_limit_mb", 0,         "output_limit_mb"},
        {"output_limit_mb", 9999,      "output_limit_mb"},
    }) {
        auto body = valid_create_body();
        body[field] = bad_value;
        auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
        if (!res) GTEST_SKIP() << "port 22307 not reachable";
        EXPECT_EQ(res->status, 400) << "field=" << field << " v=" << bad_value;
        auto j = nlohmann::json::parse(res->body);
        EXPECT_NE(j["message"].get<std::string>().find(hint), std::string::npos)
            << "field=" << field << " v=" << bad_value;
    }
    EXPECT_EQ(b.problems->create_count, 0);
}

TEST(AdminCreateHandlerTest, CasesScoreSumNot100Rejected) {
    ServerBundle b = make_server(22308);
    b.server->start();
    auto cli = make_client(22308);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    // 60 + 30 = 90 ≠ 100
    body["cases"] = nlohmann::json::array({
        {{"case_index", 1}, {"input", "1 2"}, {"expected_output", "3"},
         {"is_sample", true},  {"score", 60}},
        {{"case_index", 2}, {"input", "10 20"}, {"expected_output", "30"},
         {"is_sample", true},  {"score", 30}},
    });
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22308 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_NE(j["message"].get<std::string>().find("100"), std::string::npos);
    EXPECT_EQ(b.problems->create_count, 0);
}

TEST(AdminCreateHandlerTest, DuplicateCaseIndexRejected) {
    ServerBundle b = make_server(22309);
    b.server->start();
    auto cli = make_client(22309);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["cases"] = nlohmann::json::array({
        {{"case_index", 1}, {"input", "1 2"}, {"expected_output", "3"},
         {"is_sample", true},  {"score", 50}},
        {{"case_index", 1}, {"input", "10 20"}, {"expected_output", "30"},
         {"is_sample", true},  {"score", 50}},
    });
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22309 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_NE(j["message"].get<std::string>().find("duplicate"), std::string::npos);
}

TEST(AdminCreateHandlerTest, EmptyCasesRejected) {
    ServerBundle b = make_server(22310);
    b.server->start();
    auto cli = make_client(22310);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["cases"] = nlohmann::json::array();
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22310 not reachable";
    EXPECT_EQ(res->status, 400);
}

TEST(AdminCreateHandlerTest, UnknownTagIdRejected) {
    ServerBundle b = make_server(22311);
    b.tags->seed_tag({1, "数组", "数组"});
    b.server->start();
    auto cli = make_client(22311);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["tag_ids"] = nlohmann::json::array({1, 999});
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22311 not reachable";
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(b.problems->create_count, 0);
}

TEST(AdminCreateHandlerTest, NoTagIdsAllowed) {
    ServerBundle b = make_server(22312);
    b.server->start();
    auto cli = make_client(22312);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto body = valid_create_body();
    body["tag_ids"] = nlohmann::json::array();
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22312 not reachable";
    EXPECT_EQ(res->status, 200);
    EXPECT_EQ(b.problems->create_count, 1);
    EXPECT_EQ(b.tags->set_count, 0);  // 没动 tag
}

// ===========================================================================
//  PUT /api/admin/problems/:id
// ===========================================================================
TEST(AdminUpdateHandlerTest, UpdatesAllFields) {
    ServerBundle b = make_server(22400);
    b.tags->seed_tag({1, "数组", "数组"});
    b.tags->seed_tag({7, "动态规划", "dp"});
    Problem p; p.title = "old"; p.difficulty = Difficulty::Easy;
    p.is_published = false; p.content_md = "old"; p.created_by = 999;
    const std::int64_t pid = b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22400);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    auto body = valid_create_body();
    body["title"]        = "new";
    body["difficulty"]   = "hard";
    body["is_published"] = true;
    body["tag_ids"]      = nlohmann::json::array({1, 7});

    auto res = cli.Put("/api/admin/problems/" + std::to_string(pid), h,
                       body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22400 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["title"].get<std::string>(), "new");
    EXPECT_EQ(j["data"]["difficulty"].get<std::string>(), "hard");
    EXPECT_EQ(j["data"]["is_published"].get<bool>(), true);
    // 保留 created_by
    EXPECT_EQ(j["data"]["created_by"].get<int>(), 999);

    EXPECT_EQ(b.problems->update_count, 1);
    EXPECT_EQ(b.testcases->replace_count, 1);
    EXPECT_EQ(b.tags->set_count, 1);
}

TEST(AdminUpdateHandlerTest, NotFoundReturns1004) {
    ServerBundle b = make_server(22401);
    b.tags->seed_tag({1, "数组", "数组"});
    b.server->start();
    auto cli = make_client(22401);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto res = cli.Put("/api/admin/problems/9999", h,
                       valid_create_body().dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22401 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
    EXPECT_EQ(b.problems->update_count, 0);
}

TEST(AdminUpdateHandlerTest, ValidationErrorsSkipRepoCall) {
    ServerBundle b = make_server(22402);
    Problem p; p.title = "x"; p.difficulty = Difficulty::Easy;
    p.is_published = true; p.content_md = "x";
    const std::int64_t pid = b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22402);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    // 提交 score 总和 = 90，校验失败
    auto body = valid_create_body();
    body["cases"] = nlohmann::json::array({
        {{"case_index", 1}, {"input", "1"}, {"expected_output", "1"},
         {"is_sample", true}, {"score", 90}},
    });
    auto res = cli.Put("/api/admin/problems/" + std::to_string(pid), h,
                       body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22402 not reachable";
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(b.problems->update_count, 0);
}

// ===========================================================================
//  DELETE /api/admin/problems/:id —— 软删
// ===========================================================================
TEST(AdminDeleteHandlerTest, SoftDeleteSetsUnpublished) {
    ServerBundle b = make_server(22500);
    Problem p; p.title = "x"; p.difficulty = Difficulty::Easy;
    p.is_published = true; p.content_md = "x";
    const std::int64_t pid = b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22500);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto res = cli.Delete("/api/admin/problems/" + std::to_string(pid), h);
    if (!res) GTEST_SKIP() << "port 22500 not reachable";
    EXPECT_EQ(res->status, 200);
    auto snap = b.problems->snapshot();
    ASSERT_EQ(snap.size(), 1u);
    EXPECT_FALSE(snap[0].is_published);
    EXPECT_EQ(b.problems->soft_delete_count, 1);
}

TEST(AdminDeleteHandlerTest, NotFoundReturns1004) {
    ServerBundle b = make_server(22501);
    b.server->start();
    auto cli = make_client(22501);
    const std::string token = access_token(*b.jwt, 1, true);
    auto res = cli.Delete("/api/admin/problems/9999",
                           {{"Authorization", "Bearer " + token}});
    if (!res) GTEST_SKIP() << "port 22501 not reachable";
    EXPECT_EQ(res->status, 404);
}

// ===========================================================================
//  PATCH /api/admin/problems/:id/publish —— 上下架
// ===========================================================================
TEST(AdminPublishHandlerTest, TogglePublish) {
    ServerBundle b = make_server(22600);
    Problem p; p.title = "x"; p.difficulty = Difficulty::Easy;
    p.is_published = false; p.content_md = "x";
    const std::int64_t pid = b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22600);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token},
                          {"Content-Type",  "application/json"}};

    // false → true
    auto res = cli.Patch("/api/admin/problems/" + std::to_string(pid) + "/publish",
                          h, R"({"is_published": true})", "application/json");
    if (!res) GTEST_SKIP() << "port 22600 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["is_published"].get<bool>(), true);
    EXPECT_EQ(b.problems->set_published_count, 1);

    // true → false
    res = cli.Patch("/api/admin/problems/" + std::to_string(pid) + "/publish",
                     h, R"({"is_published": false})", "application/json");
    EXPECT_EQ(res->status, 200);
    j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["is_published"].get<bool>(), false);
    EXPECT_EQ(b.problems->set_published_count, 2);
}

TEST(AdminPublishHandlerTest, MissingIsPublishedFieldRejected) {
    ServerBundle b = make_server(22601);
    Problem p; p.title = "x"; p.difficulty = Difficulty::Easy;
    p.is_published = true; p.content_md = "x";
    const std::int64_t pid = b.problems->seed(p);
    b.server->start();
    auto cli = make_client(22601);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto res = cli.Patch("/api/admin/problems/" + std::to_string(pid) + "/publish",
                          h, R"({})", "application/json");
    if (!res) GTEST_SKIP() << "port 22601 not reachable";
    EXPECT_EQ(res->status, 400);
    EXPECT_EQ(b.problems->set_published_count, 0);
}

TEST(AdminPublishHandlerTest, NotFoundReturns1004) {
    ServerBundle b = make_server(22602);
    b.server->start();
    auto cli = make_client(22602);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};
    auto res = cli.Patch("/api/admin/problems/9999/publish", h,
                          R"({"is_published": true})", "application/json");
    if (!res) GTEST_SKIP() << "port 22602 not reachable";
    EXPECT_EQ(res->status, 404);
}

// ===========================================================================
//  DB 不可用 → 1008
// ===========================================================================
TEST(AdminHandlerDbDownTest, AllRoutesReturn1008) {
    ServerBundle b = make_server(22700);
    b.server->start();
    auto cli = make_client(22700);
    b.db_ready->store(false, std::memory_order_release);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token}};

    if (auto res = cli.Get("/api/admin/problems", h)) {
        EXPECT_EQ(res->status, 500);
        auto j = nlohmann::json::parse(res->body);
        EXPECT_EQ(j["code"].get<int>(), 1008);
    }
    if (auto res = cli.Post("/api/admin/problems", h, valid_create_body().dump(),
                              "application/json")) {
        EXPECT_EQ(res->status, 500);
    }
    if (auto res = cli.Get("/api/admin/problems/1/edit-data", h)) {
        EXPECT_EQ(res->status, 500);
    }
    if (auto res = cli.Put("/api/admin/problems/1", h, valid_create_body().dump(),
                            "application/json")) {
        EXPECT_EQ(res->status, 500);
    }
    if (auto res = cli.Delete("/api/admin/problems/1", h)) {
        EXPECT_EQ(res->status, 500);
    }
    if (auto res = cli.Patch("/api/admin/problems/1/publish", h,
                              R"({"is_published": true})", "application/json")) {
        EXPECT_EQ(res->status, 500);
    }
}

// ===========================================================================
//  端到端：admin 完整增删改
// ===========================================================================
TEST(AdminE2ETest, CreateUpdateListDeleteCycle) {
    ServerBundle b = make_server(22800);
    b.tags->seed_tag({1, "数组", "数组"});
    b.server->start();
    auto cli = make_client(22800);
    const std::string token = access_token(*b.jwt, 1, true);
    httplib::Headers h = {{"Authorization", "Bearer " + token},
                          {"Content-Type",  "application/json"}};

    // 1) CREATE
    auto body = valid_create_body();
    body["title"]        = "E2E 题";
    body["is_published"] = false;  // 先保存草稿
    auto res = cli.Post("/api/admin/problems", h, body.dump(), "application/json");
    if (!res) GTEST_SKIP() << "port 22800 not reachable";
    EXPECT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    const std::int64_t new_id = j["data"]["id"].get<std::int64_t>();
    EXPECT_FALSE(j["data"]["is_published"].get<bool>());

    // 2) LIST 能看到草稿
    res = cli.Get("/api/admin/problems", h);
    j = nlohmann::json::parse(res->body);
    bool found = false;
    for (const auto& it : j["data"]["items"]) {
        if (it["id"].get<std::int64_t>() == new_id) { found = true; break; }
    }
    EXPECT_TRUE(found);

    // 3) EDIT-DATA 能拿到全部 cases
    res = cli.Get("/api/admin/problems/" + std::to_string(new_id) + "/edit-data", h);
    j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["cases"].size(), 2u);

    // 4) UPDATE 改 title + 升 hard + 改分值
    body["title"]      = "E2E 题（修订）";
    body["difficulty"] = "hard";
    body["cases"] = nlohmann::json::array({
        {{"case_index", 1}, {"input", "1 2"}, {"expected_output", "3"},
         {"is_sample", true},  {"score", 100}},
    });
    res = cli.Put("/api/admin/problems/" + std::to_string(new_id), h,
                   body.dump(), "application.json");
    EXPECT_EQ(res->status, 200);
    j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["title"].get<std::string>(), "E2E 题（修订）");
    EXPECT_EQ(j["data"]["difficulty"].get<std::string>(), "hard");
    EXPECT_EQ(j["data"]["cases"].size(), 1u);

    // 5) PATCH publish: false → true
    res = cli.Patch("/api/admin/problems/" + std::to_string(new_id) + "/publish",
                     h, R"({"is_published": true})", "application/json");
    EXPECT_EQ(res->status, 200);
    j = nlohmann::json::parse(res->body);
    EXPECT_TRUE(j["data"]["is_published"].get<bool>());

    // 6) DELETE 软删
    res = cli.Delete("/api/admin/problems/" + std::to_string(new_id), h);
    EXPECT_EQ(res->status, 200);
    // 验证 is_published=0
    res = cli.Get("/api/admin/problems/" + std::to_string(new_id) + "/edit-data", h);
    j = nlohmann::json::parse(res->body);
    EXPECT_FALSE(j["data"]["is_published"].get<bool>());
}

}  // namespace
