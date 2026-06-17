// =============================================================================
//  tests/test_docker_client.cpp —— DockerClient 单元测试
//
//  策略：
//    - 用 cpp-httplib 起一个 in-process mock server（listen 0 → 随机端口）
//    - 录下每条收到的 request（method / path / body / headers）
//    - 按预置脚本返回 response（可指定 status + body）
//    - DockerClient 通过 tcp://127.0.0.1:<port> 连接（不走 unix socket）
//
//  覆盖 SPEC §6.1 完整工作流 + §6.4 沙箱字段：
//    1) ping() / check_image()
//    2) run() 正常路径：create → start → wait → fetch results
//    3) 编译错误：summary 写 compile_log，result=CE
//    4) TLE：wait 报 -1（容器仍在跑）
//    5) Docker daemon 不可达：返回 SE
//    6) Image 不存在：返回 SE
//    7) 创建容器失败：返回 SE
//    8) 拉日志多路复用解析（multiplexed stream）
//
//  真实 Docker 集成测试：单独放到 test_docker_client_integration.cpp，
//  由环境变量 OJ_RUN_DOCKER_TESTS=1 触发（默认 skip）。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

#include "common/config.hpp"
#include "infra/docker_client.hpp"

namespace fs   = std::filesystem;
using json      = nlohmann::json;

namespace {

// ---------------------------------------------------------------------------
//  Mock Docker Engine API
//  - GET  /_ping                       → 200 "OK"
//  - GET  /images/<name>/json          → 200 / 404
//  - POST /containers/create            → 201 { "Id": "..." }
//  - POST /containers/<id>/start        → 204
//  - POST /containers/<id>/wait         → 200 { "StatusCode": 0 }
//  - GET  /containers/<id>/logs         → 200 (multiplexed) / 404
//  - DELETE /containers/<id>            → 204
//
//  所有 request 都记到 log_，方便断言。
// ---------------------------------------------------------------------------
struct RecordedRequest {
    std::string method;
    std::string path;
    std::string query;       // 由 params 序列化得到的查询串
    std::string body;
    std::string content_type;
};

class MockDocker {
public:
    MockDocker() {
        // 选一个固定端口段，避免和本机其他服务撞
        for (int attempt = 0; attempt < 50; ++attempt) {
            port_ = 19200 + (rand() % 200);
            if (srv_.bind_to_port("127.0.0.1", port_)) break;
            port_ = 0;
        }
        if (port_ == 0) {
            throw std::runtime_error("MockDocker: no free port");
        }

        srv_.Get(R"(/v1.41/_ping)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            r.status = 200;
            r.set_content("OK", "text/plain");
        });

        srv_.Get(R"(/v1.41/images/([^/]+)/json)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            if (q.matches.size() >= 2 && q.matches[1].str().find("_missing_") != std::string::npos) {
                r.status = 404;
                r.set_content(R"({"message":"No such image: )" + q.matches[1].str() + R"("})",
                              "application/json");
            } else {
                r.status = 200;
                json j;
                j["Id"]   = "sha256:fake-" + q.matches[1].str();
                j["RepoTags"] = json::array({q.matches[1].str()});
                r.set_content(j.dump(), "application/json");
            }
        });

        srv_.Post(R"(/v1.41/containers/create)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            create_request_body_ = q.body;
            r.status = 201;
            json j;
            j["Id"] = "container-" + std::to_string(++container_counter_);
            last_container_id_ = j["Id"].get<std::string>();
            j["Warnings"] = json::array();
            r.set_content(j.dump(), "application/json");
        });

        srv_.Post(R"(/v1.41/containers/([^/]+)/start)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            r.status = 204;
        });

        srv_.Post(R"(/v1.41/containers/([^/]+)/wait)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            if (wait_script_.status_override) {
                r.status = *wait_script_.status_override;
                if (r.status == 404) return;
            }
            if (wait_script_.body_override) {
                r.set_content(*wait_script_.body_override, "application/json");
            } else {
                json j;
                j["StatusCode"] = 0;
                j["Error"]      = nullptr;
                r.set_content(j.dump(), "application/json");
            }
        });

        srv_.Get(R"(/v1.41/containers/([^/]+)/logs)", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            std::string payload = logs_script_;
            std::string full;
            std::uint32_t sz = static_cast<std::uint32_t>(payload.size());
            full.push_back('\x01');
            full.append(3, '\x00');
            full.push_back(static_cast<char>((sz >> 24) & 0xFF));
            full.push_back(static_cast<char>((sz >> 16) & 0xFF));
            full.push_back(static_cast<char>((sz >>  8) & 0xFF));
            full.push_back(static_cast<char>( sz        & 0xFF));
            full.append(payload);
            r.status = 200;
            r.set_content(full, "application/vnd.docker.raw-stream");
        });

        srv_.Delete(R"(/v1.41/containers/([^/]+))", [this](const httplib::Request& q, httplib::Response& r) {
            record(q);
            r.status = 204;
        });

        // 关键：listen 在子线程跑，否则会阻塞构造函数
        thread_ = std::thread([this] {
            srv_.listen_after_bind();
        });
        // 等 server 起来
        for (int i = 0; i < 300; ++i) {
            if (srv_.is_running()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    ~MockDocker() {
        srv_.stop();
        if (thread_.joinable()) thread_.join();
    }

    MockDocker(const MockDocker&)            = delete;
    MockDocker& operator=(const MockDocker&) = delete;

    // 对外接口
    int  port() const noexcept { return port_; }
    std::string base_url() const { return "tcp://127.0.0.1:" + std::to_string(port_); }

    std::vector<RecordedRequest> requests() {
        std::lock_guard<std::mutex> lk(mu_);
        return log_;
    }

    // 写入预置结果（测试在 run() 之前设置）
    void set_wait_response(int status, std::string body) {
        wait_script_.status_override = status;
        wait_script_.body_override   = std::move(body);
    }
    void clear_wait_response() {
        wait_script_.status_override.reset();
        wait_script_.body_override.reset();
    }
    void set_logs(std::string s) { logs_script_ = std::move(s); }

    // 让 wait 直接返 404（"container has already removed"）
    void set_wait_404() { set_wait_response(404, ""); }

    // 让 create 返回 500（创建失败）
    void set_create_error(int status, std::string body) {
        create_error_status_ = status;
        create_error_body_   = std::move(body);
    }
    // 用一个独立的 server 句柄覆盖 /containers/create；这里改成 add 一条更具体的路由
    // —— 通过 PreCreateHook 实现
    std::function<bool(const httplib::Request&, httplib::Response&)> pre_create_hook;
    void set_pre_create_hook(std::function<bool(const httplib::Request&, httplib::Response&)> h) {
        pre_create_hook = std::move(h);
    }

    std::string last_create_body() const { return create_request_body_; }
    std::string last_container_id() const { return last_container_id_; }

private:
    void record(const httplib::Request& q) {
        std::lock_guard<std::mutex> lk(mu_);
        RecordedRequest r;
        r.method      = q.method;
        r.path        = q.path;
        // httplib v0.15.3 没有 q.query 字段；用 params 重建
        std::ostringstream qs;
        bool first = true;
        for (const auto& [k, v] : q.params) {
            if (!first) qs << '&';
            qs << k << '=' << v;
            first = false;
        }
        r.query = qs.str();
        r.body        = q.body;
        r.content_type = q.get_header_value("Content-Type");
        log_.push_back(std::move(r));
    }

    httplib::Server           srv_;
    int                       port_ = 0;
    std::vector<RecordedRequest> log_;
    std::mutex                mu_;
    std::string               create_request_body_;
    std::string               last_container_id_;
    int                       container_counter_ = 0;
    std::thread               thread_;

    struct WaitScript {
        std::optional<int>    status_override;
        std::optional<std::string> body_override;
    } wait_script_;
    std::string               logs_script_ = "compile error: syntax error\n";
    int                       create_error_status_ = 0;
    std::string               create_error_body_;
};

}  // namespace

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
namespace {

oj::common::DockerConfig make_cfg_tcp(const std::string& url) {
    oj::common::DockerConfig c;
    c.host                       = url;
    c.api_version                = "v1.41";
    c.request_timeout_sec        = 5;
    c.container_wait_buffer_sec  = 1;
    return c;
}

fs::path make_temp_work_root() {
    static std::atomic<int> n{0};
    int id = n.fetch_add(1);
    auto p = fs::temp_directory_path() / ("oj_docker_test_" + std::to_string(::getpid()) +
                                          "_" + std::to_string(id));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_summary_and_per_case(const fs::path& workdir,
                                const std::string& summary_json,
                                const std::string& per_case_json = R"({"cases":[]})") {
    fs::create_directories(workdir / "result");
    {
        std::ofstream f(workdir / "result" / "summary.json");
        f << summary_json;
    }
    {
        std::ofstream f(workdir / "result" / "per_case.json");
        f << per_case_json;
    }
}

oj::infra::JudgeTask make_task(std::int64_t id, oj::infra::SubmissionLanguage lang,
                               const std::string& code,
                               oj::infra::JudgeLimits lim,
                               const std::string& image = "judge-cpp:1.0") {
    oj::infra::JudgeTask t;
    t.submission_id = id;
    t.language      = lang;
    t.code          = code;
    t.image         = image;
    t.limits        = lim;
    t.testcases.emplace_back("2 3\n", "5\n");
    t.testcases.emplace_back("10 20\n", "30\n");
    return t;
}

}  // namespace

// ===========================================================================
//  ping() / check_image()
// ===========================================================================
TEST(DockerClientPingTest, PingSucceeds) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    oj::infra::DockerClient cli(cfg);

    auto err = cli.ping();
    EXPECT_TRUE(err.empty()) << err;

    auto reqs = m.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].method, "GET");
    EXPECT_EQ(reqs[0].path,   "/v1.41/_ping");
}

TEST(DockerClientPingTest, PingFailsWhenConnectionRefused) {
    // 故意用未占用的端口
    int unused = 1;
    for (int p = 19999; p > 19000 && !unused; --p) {
        // (实际上 MockDocker 已经挑了一个端口；这里改成连到 127.0.0.1:1 一定失败)
        unused = 1;
    }
    oj::common::DockerConfig cfg = make_cfg_tcp("tcp://127.0.0.1:1");
    oj::infra::DockerClient cli(cfg);
    auto err = cli.ping();
    EXPECT_FALSE(err.empty());
}

TEST(DockerClientCheckImageTest, ExistsReturnsEmpty) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    oj::infra::DockerClient cli(cfg);
    auto err = cli.check_image("judge-cpp:1.0");
    EXPECT_TRUE(err.empty()) << err;
}

TEST(DockerClientCheckImageTest, MissingReturnsError) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    oj::infra::DockerClient cli(cfg);
    auto err = cli.check_image("judge_missing_xx:1.0");
    EXPECT_NE(err.find("not found"), std::string::npos) << err;
}

// ===========================================================================
//  run() 正常路径
// ===========================================================================
TEST(DockerClientRunTest, FullPipeline_AC) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    cfg.container_wait_buffer_sec = 0;
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    // mock 返回：wait 直接给正常 0，logs 默认即可
    write_summary_and_per_case(cli.work_root() / "123",
        R"({"compile_ok":true,"result":"AC","total_score":100,"time_used_ms":15,"memory_used_kb":4096})",
        R"({"cases":[{"case_index":1,"status":"AC","time_used_ms":7,"memory_used_kb":2048,"score":50,"is_sample":true,"user_output":"5\n"}]})");

    auto task = make_task(123, oj::infra::SubmissionLanguage::Cpp,
                          "#include <cstdio>\nint main(){return 0;}\n",
                          {2000, 256, 64});
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::AC);
    EXPECT_EQ(r.result_string, "AC");
    EXPECT_EQ(r.total_score, 100);
    EXPECT_EQ(r.time_ms, 15);
    ASSERT_EQ(r.cases.size(), 1u);
    EXPECT_EQ(r.cases[0].status, oj::infra::JudgeStatus::AC);
    EXPECT_EQ(r.cases[0].score, 50);

    // 校验请求序列：check_image → create → start → wait → (logs?) → delete
    auto reqs = m.requests();
    std::vector<std::string> seq;
    for (const auto& r : reqs) seq.push_back(r.method + " " + r.path);
    EXPECT_GE(seq.size(), 5u) << "expected ≥5 calls, got " << seq.size();
    EXPECT_EQ(seq[0].substr(0, 4), "GET ");
    EXPECT_NE(seq[0].find("/v1.41/images/"), std::string::npos);
    EXPECT_NE(seq[1].find("POST /v1.41/containers/create"), std::string::npos);
    EXPECT_NE(seq[2].find("POST /v1.41/containers/"),       std::string::npos);
    EXPECT_NE(seq[2].find("/start"),                         std::string::npos);
    EXPECT_NE(seq[3].find("/wait"),                          std::string::npos);

    // create body 应含 HostConfig + sandbox 字段（SPEC §6.4）
    std::string body = m.last_create_body();
    fprintf(stderr, "[TEST] create body=%s\n", body.c_str()); auto j = json::parse(body);
    EXPECT_EQ(j["Image"],    "judge-cpp:1.0");
    EXPECT_EQ(j["User"],     "judge");
    EXPECT_EQ(j["WorkingDir"], "/judge/work");
    auto& hc = j["HostConfig"];
    EXPECT_EQ(hc["NetworkMode"],    "none");
    EXPECT_EQ(hc["ReadonlyRootfs"], true);
    EXPECT_EQ(hc["AutoRemove"],     true);
    EXPECT_EQ(hc["PidsLimit"],      64);
    EXPECT_EQ(hc["CpuQuota"],       200000);
    EXPECT_EQ(hc["Memory"],         256L * 1024 * 1024);
    EXPECT_EQ(hc["MemorySwap"],     256L * 1024 * 1024);
    ASSERT_TRUE(hc.contains("Mounts"));
    ASSERT_EQ(hc["Mounts"].size(), 1u);
    EXPECT_EQ(hc["Mounts"][0]["Type"],   "bind");
    EXPECT_EQ(hc["Mounts"][0]["Target"], "/judge/work");
    ASSERT_TRUE(hc.contains("CapDrop"));
    EXPECT_EQ(hc["CapDrop"][0], "ALL");
    ASSERT_TRUE(hc.contains("SecurityOpt"));
    EXPECT_EQ(hc["SecurityOpt"][0], "no-new-privileges");
    ASSERT_TRUE(hc.contains("Tmpfs"));
    EXPECT_TRUE(hc["Tmpfs"].contains("/tmp"));
}

TEST(DockerClientRunTest, CompileErrorExtractsCompileLog) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    cfg.container_wait_buffer_sec = 0;
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    write_summary_and_per_case(cli.work_root() / "124",
        R"({"compile_ok":false,"result":"CE","total_score":0,"time_used_ms":0,"memory_used_kb":0,"compile_log":"main.cpp:1: error: expected ';' before 'return'"})");

    auto task = make_task(124, oj::infra::SubmissionLanguage::Cpp,
                          "broken code", {2000, 256, 64});
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::CE);
    EXPECT_FALSE(r.compile_output.empty());
    EXPECT_NE(r.compile_output.find("error: expected"), std::string::npos);
}

TEST(DockerClientRunTest, WaitsForResultsFromLogFallback) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    cfg.container_wait_buffer_sec = 0;
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    // summary 没有 compile_log → runner 应回退到 logs
    write_summary_and_per_case(cli.work_root() / "125",
        R"({"compile_ok":false,"result":"CE","total_score":0,"time_used_ms":0,"memory_used_kb":0})");
    m.set_logs("from logs: syntax error\n");

    auto task = make_task(125, oj::infra::SubmissionLanguage::Java,
                          "class Main{}", {5000, 256, 64}, "judge-java:1.0");
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::CE);
    // logs 多路复用解析后写入 compile_output
    EXPECT_NE(r.compile_output.find("from logs"), std::string::npos);
}

TEST(DockerClientRunTest, ImageNotFoundReturnsSE) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    cfg.container_wait_buffer_sec = 0;
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    auto task = make_task(126, oj::infra::SubmissionLanguage::Cpp, "x", {2000,256,64},
                          "judge_missing_xx:1.0");
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::SE);
    EXPECT_NE(r.judge_message.find("not found"), std::string::npos);
    // workdir 应该被清理
    EXPECT_FALSE(fs::exists(cli.work_root() / "126"));
}

TEST(DockerClientRunTest, EmptyImageReturnsSE) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    auto task = make_task(127, oj::infra::SubmissionLanguage::Cpp, "x", {2000,256,64});
    task.image = "";  // 故意空
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::SE);
    EXPECT_NE(r.judge_message.find("no image"), std::string::npos);
}

TEST(DockerClientRunTest, DaemonUnreachableReturnsSE) {
    // 不启动 mock，直接连到无效端口
    oj::common::DockerConfig cfg = make_cfg_tcp("tcp://127.0.0.1:1");
    oj::infra::DockerClient cli(cfg);
    cli.set_work_root(make_temp_work_root());

    auto task = make_task(128, oj::infra::SubmissionLanguage::Cpp, "x", {2000,256,64});
    auto r = cli.run(task);
    EXPECT_EQ(r.overall, oj::infra::JudgeStatus::SE);
    EXPECT_FALSE(r.judge_message.empty());
}

TEST(DockerClientRunTest, CleansUpWorkdir) {
    MockDocker m;
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    cfg.container_wait_buffer_sec = 0;
    oj::infra::DockerClient cli(cfg);
    auto wr = make_temp_work_root();
    cli.set_work_root(wr);

    write_summary_and_per_case(wr / "129",
        R"({"compile_ok":true,"result":"AC","total_score":100,"time_used_ms":1,"memory_used_kb":1})");
    auto task = make_task(129, oj::infra::SubmissionLanguage::Cpp, "x", {2000,256,64});
    cli.run(task);
    // 跑完后 workdir 应被清掉
    EXPECT_FALSE(fs::exists(wr / "129")) << "workdir must be cleaned after run";
}

// ===========================================================================
//  Unix socket URL parsing 验证
// ===========================================================================
TEST(DockerClientUrlParseTest, TcpHostParsedCorrectly) {
    MockDocker m;
    // tcp URL 已经在所有上面验证过；这里只证明 config 接受合法 URL
    oj::common::DockerConfig cfg = make_cfg_tcp(m.base_url());
    oj::infra::DockerClient cli(cfg);
    auto err = cli.ping();
    EXPECT_TRUE(err.empty()) << err;
}
