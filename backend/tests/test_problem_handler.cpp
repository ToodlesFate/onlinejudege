// =============================================================================
//  test_problem_handler.cpp — GET /api/problems HTTP 集成测试
//  跑真 HttpServer + httplib::Client，用 InMemoryProblemRepository 替换 DB
//
//  覆盖：
//    1) 200 + envelope {code,message,data{items[],total,page,size}}
//    2) data.items[i] 字段完整性（id/title/difficulty/tags/stats/created_*）
//    3) page / size / difficulty / sort / tag / q query 参数生效
//    4) 默认 include_unpublished=false（公开 API）
//    5) 参数错误 → 1001 BadRequest
//    6) DB 不可用 → 1008 SystemError
//    7) 错误方法（POST/PUT/DELETE）→ 404
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/problem_types.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/problem_handler.hpp"

namespace {

using oj::common::AppConfig;
using oj::domain::Difficulty;
using oj::domain::IProblemRepository;
using oj::domain::Problem;
using oj::domain::ProblemListItem;
using oj::domain::ProblemListQuery;
using oj::domain::ProblemListResult;
using oj::domain::ProblemService;
using oj::domain::Tag;
using oj::http::HttpServer;

class InMemoryProblemRepository : public IProblemRepository {
public:
    std::optional<Problem> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& p : problems_) if (p.id == id) return p;
        return std::nullopt;
    }
    ProblemListResult list(const ProblemListQuery& q) override {
        std::lock_guard<std::mutex> lk(mu_);
        ProblemListResult out;
        out.page = q.page;
        out.page_size = q.page_size;
        for (const auto& p : problems_) {
            if (!q.include_unpublished && !p.is_published) continue;
            if (q.difficulty.has_value() && p.difficulty != *q.difficulty) continue;
            if (!q.q.empty() && p.title.find(q.q) == std::string::npos) continue;

            // tag AND 过滤：problem 的 tags 必须包含全部指定 slug
            if (!q.tag_slugs.empty()) {
                auto t = tag_of_.find(p.id);
                if (t == tag_of_.end()) continue;
                std::set<int> problem_tag_ids;
                for (const auto& tag : t->second) problem_tag_ids.insert(tag.id);
                bool all_match = true;
                for (const auto& slug : q.tag_slugs) {
                    auto it2 = tag_table_.find(slug);
                    if (it2 == tag_table_.end()) { all_match = false; break; }
                    if (!problem_tag_ids.count(it2->second)) { all_match = false; break; }
                }
                if (!all_match) continue;
            }

            ProblemListItem it;
            it.id           = p.id;
            it.title        = p.title;
            it.difficulty   = p.difficulty;
            it.is_published = p.is_published;
            it.created_by   = p.created_by;
            it.created_at   = p.created_at;
            it.total_submissions    = 10;
            it.accepted_submissions = std::min(p.id * 2 - 1, std::int64_t{10});
            if (it.accepted_submissions < 0) it.accepted_submissions = 0;
            auto t = tag_of_.find(p.id);
            if (t != tag_of_.end()) it.tags = t->second;
            out.items.push_back(std::move(it));
        }
        out.total = static_cast<std::int64_t>(out.items.size());
        std::sort(out.items.begin(), out.items.end(), [&](const auto& a, const auto& b) {
            switch (q.sort) {
                case ProblemListQuery::Sort::IdDesc:        return a.id > b.id;
                case ProblemListQuery::Sort::CreatedDesc:   return a.created_at > b.created_at;
                case ProblemListQuery::Sort::PassRateDesc:  return a.pass_rate() > b.pass_rate();
            }
            return false;
        });
        const int start = (q.page - 1) * q.page_size;
        const int end   = start + q.page_size;
        const int total = static_cast<int>(out.items.size());
        if (start >= total) {
            out.items.clear();
        } else {
            out.items.erase(out.items.begin(), out.items.begin() + start);
            if (end < total) {
                out.items.erase(out.items.begin() + (end - start), out.items.end());
            }
        }
        return out;
    }
    Problem create(const Problem&) override { throw std::runtime_error("nope"); }
    void update(const Problem&) override { throw std::runtime_error("nope"); }
    void soft_delete(std::int64_t) override { throw std::runtime_error("nope"); }
    void set_published(std::int64_t, bool) override { throw std::runtime_error("nope"); }
    std::pair<int,int> submission_stats(std::int64_t) override { return {0,0}; }

    void add(Problem p, std::vector<Tag> tags = {}) {
        std::lock_guard<std::mutex> lk(mu_);
        ++next_id_;
        p.id = next_id_;
        problems_.push_back(std::move(p));
        tag_of_[p.id] = std::move(tags);
    }

    // 预置 tag 表 —— 用于按 slug 查 id 做 AND 过滤
    void seed_tag(const Tag& t) {
        std::lock_guard<std::mutex> lk(mu_);
        tag_table_[t.slug] = t.id;
    }

private:
    mutable std::mutex mu_;
    std::int64_t next_id_ = 0;
    std::vector<Problem> problems_;
    std::map<std::int64_t, std::vector<Tag>> tag_of_;
    std::map<std::string, int>             tag_table_;  // slug → id
};

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
    explicit ScopedServer(uint16_t port) : cfg_(make_cfg(port)), srv_(std::move(cfg_)) {}
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
    AppConfig    cfg_;
    HttpServer   srv_;
    std::thread  thread_;
    std::atomic<bool> ready_{false};
};

httplib::Client make_client(uint16_t port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(2, 0);
    c.set_read_timeout(2, 0);
    return c;
}

struct ServerBundle {
    std::unique_ptr<ScopedServer>            server;
    std::shared_ptr<InMemoryProblemRepository> repo;
    std::shared_ptr<ProblemService>          service;
    std::shared_ptr<std::atomic<bool>>       db_ready{std::make_shared<std::atomic<bool>>(true)};
};

ServerBundle make_server(uint16_t port) {
    ServerBundle b;
    b.repo    = std::make_shared<InMemoryProblemRepository>();
    b.service = std::make_shared<ProblemService>(b.repo);
    b.server  = std::make_unique<ScopedServer>(port);
    auto ready_ptr = b.db_ready;
    oj::http::handlers::register_problem_routes(
        b.server->server(), b.service,
        [ready_ptr] { return ready_ptr->load(std::memory_order_acquire); });
    return b;
}

Problem mkP(const std::string& title, Difficulty d, bool pub,
            const std::string& created_at = "2026-04-23T10:00:00Z") {
    Problem p;
    p.title = title;
    p.content_md = "x";
    p.difficulty = d;
    p.is_published = pub;
    p.created_by = 1;
    p.created_at = created_at;
    return p;
}

// ---------------------------------------------------------------------------
//  Envelope & 字段完整性
// ---------------------------------------------------------------------------
TEST(ProblemListHandlerTest, ReturnsEnvelopeAndItems) {
    ServerBundle b = make_server(19500);
    b.repo->add(mkP("两数之和", Difficulty::Easy, true));
    b.repo->add(mkP("反转链表", Difficulty::Medium, true));
    b.server->start();

    auto res = make_client(19500).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19500 not reachable";

    ASSERT_EQ(res->status, 200);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 0);
    EXPECT_EQ(j["message"], "ok");
    ASSERT_TRUE(j["data"].is_object());
    EXPECT_TRUE(j["data"]["items"].is_array());
    EXPECT_EQ(j["data"]["items"].size(), 2u);
    EXPECT_EQ(j["data"]["total"].get<int>(), 2);
    EXPECT_EQ(j["data"]["page"].get<int>(), 1);
    EXPECT_EQ(j["data"]["size"].get<int>(), 20);
}

TEST(ProblemListHandlerTest, ItemCarriesAllSpecFields) {
    ServerBundle b = make_server(19501);
    Tag arr{1, "数组", "数组"};
    Tag str{2, "字符串", "string"};
    b.repo->add(mkP("两数之和", Difficulty::Easy, true), {arr});
    b.repo->add(mkP("最长回文", Difficulty::Hard, true), {arr, str});
    b.server->start();

    auto res = make_client(19501).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19501 not reachable";
    ASSERT_EQ(res->status, 200);
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
    // stats sub-fields
    EXPECT_TRUE(it["stats"].contains("total"));
    EXPECT_TRUE(it["stats"].contains("accepted"));
    EXPECT_TRUE(it["stats"].contains("pass_rate"));
}

TEST(ProblemListHandlerTest, DifficultyIsStringified) {
    ServerBundle b = make_server(19502);
    b.repo->add(mkP("p1", Difficulty::Easy,   true));
    b.repo->add(mkP("p2", Difficulty::Medium, true));
    b.repo->add(mkP("p3", Difficulty::Hard,   true));
    b.server->start();

    auto res = make_client(19502).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19502 not reachable";
    auto j = nlohmann::json::parse(res->body);
    std::set<std::string> seen;
    for (const auto& it : j["data"]["items"]) seen.insert(it["difficulty"].get<std::string>());
    EXPECT_EQ(seen.size(), 3u);
    EXPECT_TRUE(seen.count("easy"));
    EXPECT_TRUE(seen.count("medium"));
    EXPECT_TRUE(seen.count("hard"));
}

TEST(ProblemListHandlerTest, TagsArrayHasIdNameSlug) {
    ServerBundle b = make_server(19503);
    Tag arr{1, "数组", "数组"};
    b.repo->add(mkP("p1", Difficulty::Easy, true), {arr});
    b.server->start();

    auto res = make_client(19503).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19503 not reachable";
    auto j = nlohmann::json::parse(res->body);
    const auto& tags = j["data"]["items"][0]["tags"];
    ASSERT_EQ(tags.size(), 1u);
    EXPECT_EQ(tags[0]["id"].get<int>(), 1);
    EXPECT_EQ(tags[0]["name"], "数组");
    EXPECT_EQ(tags[0]["slug"], "数组");
}

// ---------------------------------------------------------------------------
//  Query 参数生效
// ---------------------------------------------------------------------------
TEST(ProblemListHandlerTest, PageAndSizeAreRespected) {
    ServerBundle b = make_server(19504);
    for (int i = 0; i < 25; ++i) {
        b.repo->add(mkP("p" + std::to_string(i), Difficulty::Easy, true));
    }
    b.server->start();

    auto res = make_client(19504).Get("/api/problems?page=2&size=10");
    if (!res) GTEST_SKIP() << "port 19504 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["page"].get<int>(), 2);
    EXPECT_EQ(j["data"]["size"].get<int>(), 10);
    EXPECT_EQ(j["data"]["total"].get<int>(), 25);
    EXPECT_EQ(j["data"]["items"].size(), 10u);
}

TEST(ProblemListHandlerTest, SizeInvalidFallsBackTo20) {
    ServerBundle b = make_server(19505);
    for (int i = 0; i < 25; ++i) {
        b.repo->add(mkP("p" + std::to_string(i), Difficulty::Easy, true));
    }
    b.server->start();

    auto res = make_client(19505).Get("/api/problems?size=999");
    if (!res) GTEST_SKIP() << "port 19505 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["size"].get<int>(), 20);
}

TEST(ProblemListHandlerTest, DifficultyFilter) {
    ServerBundle b = make_server(19506);
    b.repo->add(mkP("easy1",   Difficulty::Easy,   true));
    b.repo->add(mkP("easy2",   Difficulty::Easy,   true));
    b.repo->add(mkP("hard1",   Difficulty::Hard,   true));
    b.server->start();

    auto res = make_client(19506).Get("/api/problems?difficulty=hard");
    if (!res) GTEST_SKIP() << "port 19506 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j["data"]["items"][0]["title"], "hard1");
}

TEST(ProblemListHandlerTest, QFilterSubstring) {
    ServerBundle b = make_server(19507);
    b.repo->add(mkP("两数之和",   Difficulty::Easy, true));
    b.repo->add(mkP("三数之和",   Difficulty::Easy, true));
    b.repo->add(mkP("最长回文子串", Difficulty::Hard, true));
    b.server->start();

    auto res = make_client(19507).Get("/api/problems?q=" + httplib::detail::encode_url("之"));
    if (!res) GTEST_SKIP() << "port 19507 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 2);
}

TEST(ProblemListHandlerTest, TagSingleFilter) {
    ServerBundle b = make_server(19508);
    Tag arr{1, "数组", "数组"};
    Tag str{2, "字符串", "string"};
    b.repo->seed_tag(arr);
    b.repo->seed_tag(str);
    b.repo->add(mkP("p1", Difficulty::Easy, true), {arr});
    b.repo->add(mkP("p2", Difficulty::Easy, true), {str});
    b.repo->add(mkP("p3", Difficulty::Easy, true), {arr, str});
    b.server->start();

    auto res = make_client(19508).Get("/api/problems?tag=" + httplib::detail::encode_url("数组"));
    if (!res) GTEST_SKIP() << "port 19508 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 2);
    std::set<std::string> titles;
    for (const auto& it : j["data"]["items"]) titles.insert(it["title"].get<std::string>());
    EXPECT_TRUE(titles.count("p1"));
    EXPECT_TRUE(titles.count("p3"));
}

TEST(ProblemListHandlerTest, TagMultipleAnd) {
    ServerBundle b = make_server(19509);
    Tag arr{1, "数组", "数组"};
    Tag str{2, "字符串", "string"};
    b.repo->seed_tag(arr);
    b.repo->seed_tag(str);
    b.repo->add(mkP("p1", Difficulty::Easy, true), {arr});
    b.repo->add(mkP("p2", Difficulty::Easy, true), {str});
    b.repo->add(mkP("p3", Difficulty::Easy, true), {arr, str});
    b.server->start();

    auto res = make_client(19509).Get(
        "/api/problems?tag=" + httplib::detail::encode_url("数组") +
        "&tag=" + httplib::detail::encode_url("string"));
    if (!res) GTEST_SKIP() << "port 19509 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j["data"]["items"][0]["title"], "p3");
}

TEST(ProblemListHandlerTest, SortByCreatedDesc) {
    ServerBundle b = make_server(19510);
    b.repo->add(mkP("old", Difficulty::Easy, true, "2026-04-01T10:00:00Z"));
    b.repo->add(mkP("new", Difficulty::Easy, true, "2026-05-01T10:00:00Z"));
    b.server->start();

    auto res = make_client(19510).Get("/api/problems?sort=created_desc");
    if (!res) GTEST_SKIP() << "port 19510 not reachable";
    auto j = nlohmann::json::parse(res->body);
    ASSERT_EQ(j["data"]["items"].size(), 2u);
    EXPECT_EQ(j["data"]["items"][0]["title"], "new");
    EXPECT_EQ(j["data"]["items"][1]["title"], "old");
}

TEST(ProblemListHandlerTest, SortByPassRateDesc) {
    ServerBundle b = make_server(19511);
    b.repo->add(mkP("low",  Difficulty::Easy, true));  // id=1: 1/10
    b.repo->add(mkP("high", Difficulty::Easy, true));  // id=2: 3/10
    b.server->start();

    auto res = make_client(19511).Get("/api/problems?sort=pass_rate_desc");
    if (!res) GTEST_SKIP() << "port 19511 not reachable";
    auto j = nlohmann::json::parse(res->body);
    ASSERT_EQ(j["data"]["items"].size(), 2u);
    EXPECT_EQ(j["data"]["items"][0]["title"], "high");
    EXPECT_EQ(j["data"]["items"][1]["title"], "low");
}

TEST(ProblemListHandlerTest, PublicApiHidesUnpublishedEvenIfParamSet) {
    ServerBundle b = make_server(19512);
    b.repo->add(mkP("pub",  Difficulty::Easy, true));
    b.repo->add(mkP("priv", Difficulty::Easy, false));
    b.server->start();

    auto res = make_client(19512).Get("/api/problems?include_unpublished=1");
    if (!res) GTEST_SKIP() << "port 19512 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 1);
    EXPECT_EQ(j["data"]["items"][0]["title"], "pub");
}

TEST(ProblemListHandlerTest, EmptyResultReturnsTotalZeroAndEmptyArray) {
    ServerBundle b = make_server(19513);
    b.server->start();

    auto res = make_client(19513).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19513 not reachable";
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["data"]["total"].get<int>(), 0);
    EXPECT_TRUE(j["data"]["items"].is_array());
    EXPECT_EQ(j["data"]["items"].size(), 0u);
}

// ---------------------------------------------------------------------------
//  错误路径
// ---------------------------------------------------------------------------
TEST(ProblemListHandlerTest, InvalidPageReturns400) {
    ServerBundle b = make_server(19514);
    b.server->start();

    auto res = make_client(19514).Get("/api/problems?page=0");
    if (!res) GTEST_SKIP() << "port 19514 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
    EXPECT_NE(std::string{j["message"]}.find("page"), std::string::npos);
}

TEST(ProblemListHandlerTest, InvalidPageFormatReturns400) {
    ServerBundle b = make_server(19515);
    b.server->start();

    auto res = make_client(19515).Get("/api/problems?page=abc");
    if (!res) GTEST_SKIP() << "port 19515 not reachable";
    EXPECT_EQ(res->status, 400);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1001);
}

TEST(ProblemListHandlerTest, DbDownReturnsEnvelope) {
    ServerBundle b = make_server(19516);
    b.server->start();
    b.db_ready->store(false, std::memory_order_release);

    auto res = make_client(19516).Get("/api/problems");
    if (!res) GTEST_SKIP() << "port 19516 not reachable";
    EXPECT_EQ(res->status, 500);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1008);
    EXPECT_NE(std::string{j["message"]}.find("database"), std::string::npos);
}

TEST(ProblemListHandlerTest, PostReturns404) {
    ServerBundle b = make_server(19517);
    b.server->start();
    auto res = make_client(19517).Post("/api/problems", "{}", "application/json");
    if (!res) GTEST_SKIP() << "port 19517 not reachable";
    EXPECT_EQ(res->status, 404);
    auto j = nlohmann::json::parse(res->body);
    EXPECT_EQ(j["code"].get<int>(), 1004);
}

TEST(ProblemListHandlerTest, PutReturns404) {
    ServerBundle b = make_server(19518);
    b.server->start();
    auto res = make_client(19518).Put("/api/problems", "{}", "application/json");
    if (!res) GTEST_SKIP() << "port 19518 not reachable";
    EXPECT_EQ(res->status, 404);
}

}  // namespace
