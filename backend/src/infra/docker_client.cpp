#include "infra/docker_client.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <curl/curl.h>

namespace oj::infra {

namespace fs = std::filesystem;
using nlohmann::json;

// =============================================================================
//  状态/语言字符串
// =============================================================================
const char* to_string(JudgeStatus s) noexcept {
    switch (s) {
        case JudgeStatus::AC:  return "AC";
        case JudgeStatus::WA:  return "WA";
        case JudgeStatus::TLE: return "TLE";
        case JudgeStatus::MLE: return "MLE";
        case JudgeStatus::OLE: return "OLE";
        case JudgeStatus::RE:  return "RE";
        case JudgeStatus::CE:  return "CE";
        case JudgeStatus::SE:  return "SE";
    }
    return "SE";
}

JudgeStatus status_from_string(std::string_view s) noexcept {
    if (s == "AC")  return JudgeStatus::AC;
    if (s == "WA")  return JudgeStatus::WA;
    if (s == "TLE") return JudgeStatus::TLE;
    if (s == "MLE") return JudgeStatus::MLE;
    if (s == "OLE") return JudgeStatus::OLE;
    if (s == "RE")  return JudgeStatus::RE;
    if (s == "CE")  return JudgeStatus::CE;
    if (s == "SE")  return JudgeStatus::SE;
    return JudgeStatus::SE;
}

SubmissionLanguage language_from_string(std::string_view s) noexcept {
    if (s == "c")      return SubmissionLanguage::C;
    if (s == "cpp")    return SubmissionLanguage::Cpp;
    if (s == "java")   return SubmissionLanguage::Java;
    if (s == "python") return SubmissionLanguage::Python;
    if (s == "go")     return SubmissionLanguage::Go;
    return SubmissionLanguage::Unknown;
}

const char* language_to_string(SubmissionLanguage l) noexcept {
    switch (l) {
        case SubmissionLanguage::C:      return "c";
        case SubmissionLanguage::Cpp:    return "cpp";
        case SubmissionLanguage::Java:   return "java";
        case SubmissionLanguage::Python: return "python";
        case SubmissionLanguage::Go:     return "go";
        default:                         return "unknown";
    }
}

// =============================================================================
//  libcurl 全局 init —— 程序生命周期内调一次
// =============================================================================
namespace {

struct CurlGlobalInit {
    CurlGlobalInit()  { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobalInit() { curl_global_cleanup(); }
};

CurlGlobalInit& curl_global() {
    static CurlGlobalInit g;
    return g;
}

size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// Unix socket URL 解析：
//   "unix:///var/run/docker.sock" → host=localhost, unix_path=/var/run/docker.sock
//   "tcp://127.0.0.1:2375"        → url="http://127.0.0.1:2375/..."
struct ParsedUrl {
    std::string url;        // 给 libcurl 的 URL
    std::string unix_path;  // 给 CURLOPT_UNIX_SOCKET_PATH
};

ParsedUrl parse_docker_host(const std::string& host_in, const std::string& path_query) {
    ParsedUrl out;
    if (host_in.rfind("unix://", 0) == 0) {
        out.unix_path = host_in.substr(7);
        out.url = "http://localhost" + path_query;
    } else if (host_in.rfind("tcp://", 0) == 0) {
        out.url = "http://" + host_in.substr(6) + path_query;
    } else {
        out.unix_path = host_in;
        out.url = "http://localhost" + path_query;
    }
    return out;
}

std::string truncate(std::string s, std::size_t max_len) {
    if (s.size() <= max_len) return s;
    s.resize(max_len);
    if (max_len >= 3) s.replace(max_len - 3, 3, "...");
    return s;
}

std::string slurp_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, std::string_view content) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    if (!content.empty()) f.write(content.data(), static_cast<std::streamsize>(content.size()));
    return f.good();
}

// docker 路径前缀：/v1.41/containers/...
constexpr const char* kApiBase = "/v1.41";

}  // namespace

// =============================================================================
//  DockerClient 主体
// =============================================================================
DockerClient::DockerClient(common::DockerConfig cfg)
    : docker_cfg_(std::move(cfg)), work_root_("/tmp/oj") {
    curl_global();
}

DockerClient::~DockerClient() = default;

std::string DockerClient::image_for(SubmissionLanguage /*l*/) const {
    return "";  // run() 从 task.image 取
}

std::string DockerClient::ping() const {
    auto r = http_get("/_ping");
    if (!r.error.empty()) return "ping failed: " + r.error;
    if (r.status != 200) {
        return "ping status=" + std::to_string(r.status) +
               " body=" + truncate(r.body, 200);
    }
    return {};
}

std::string DockerClient::check_image(std::string_view image) const {
    char* esc = curl_easy_escape(nullptr, std::string(image).c_str(), 0);
    std::string encoded = esc ? esc : std::string(image);
    if (esc) curl_free(esc);
    std::string path = "/images/" + encoded + "/json";
    auto r = http_get(path);
    if (!r.error.empty()) return "image check failed: " + r.error;
    if (r.status == 200) return {};
    if (r.status == 404) return "image not found: " + std::string(image);
    return "image check status=" + std::to_string(r.status) +
           " body=" + truncate(r.body, 200);
}

// =============================================================================
//  HTTP 调用
// =============================================================================
DockerClient::HttpResponse DockerClient::http_get(std::string_view path) const {
    HttpResponse out;
    CURL* h = curl_easy_init();
    if (!h) { out.error = "curl_easy_init failed"; return out; }

    auto u = parse_docker_host(docker_cfg_.host, std::string(kApiBase) + std::string(path));
    std::string url_owned(u.url);
    std::string unix_path_owned(u.unix_path);
    curl_easy_setopt(h, CURLOPT_URL, url_owned.c_str());
    if (!unix_path_owned.empty()) curl_easy_setopt(h, CURLOPT_UNIX_SOCKET_PATH, unix_path_owned.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT,        static_cast<long>(docker_cfg_.request_timeout_sec));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,      &out.body);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL,        1L);

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) out.error = curl_easy_strerror(rc);
    else {
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        out.status = status;
    }
    curl_easy_cleanup(h);
    return out;
}

DockerClient::HttpResponse DockerClient::http_post(std::string_view path, std::string_view body,
                                                 std::string_view content_type) const {
    HttpResponse out;
    CURL* h = curl_easy_init();
    if (!h) { out.error = "curl_easy_init failed"; return out; }

    auto u = parse_docker_host(docker_cfg_.host, std::string(kApiBase) + std::string(path));
    curl_easy_setopt(h, CURLOPT_URL, u.url.c_str());
    if (!u.unix_path.empty()) curl_easy_setopt(h, CURLOPT_UNIX_SOCKET_PATH, u.unix_path.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT,        static_cast<long>(docker_cfg_.request_timeout_sec));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,      &out.body);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(h, CURLOPT_POST,           1L);

    // 关键：把 std::string_view 物化为 std::string，**保证它在 curl_easy_perform 期间仍然 alive**
    // 否则 std::string(body).c_str() 是个 dangling pointer，libcurl 会读到乱码
    std::string body_owned(body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS,     body_owned.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE,  static_cast<long>(body_owned.size()));

    std::string url_owned(u.url);            // 同理 URL
    curl_easy_setopt(h, CURLOPT_URL, url_owned.c_str());
    std::string unix_path_owned(u.unix_path);
    if (!unix_path_owned.empty()) {
        curl_easy_setopt(h, CURLOPT_UNIX_SOCKET_PATH, unix_path_owned.c_str());
    }

    struct curl_slist* hdrs = nullptr;
    std::string ct = "Content-Type: ";
    ct += std::string(content_type);
    hdrs = curl_slist_append(hdrs, ct.c_str());
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) out.error = curl_easy_strerror(rc);
    else {
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        out.status = status;
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    return out;
}

DockerClient::HttpResponse DockerClient::http_delete(std::string_view path) const {
    HttpResponse out;
    CURL* h = curl_easy_init();
    if (!h) { out.error = "curl_easy_init failed"; return out; }

    auto u = parse_docker_host(docker_cfg_.host, std::string(kApiBase) + std::string(path));
    std::string url_owned(u.url);
    std::string unix_path_owned(u.unix_path);
    curl_easy_setopt(h, CURLOPT_URL, url_owned.c_str());
    if (!unix_path_owned.empty()) curl_easy_setopt(h, CURLOPT_UNIX_SOCKET_PATH, unix_path_owned.c_str());
    curl_easy_setopt(h, CURLOPT_TIMEOUT,        static_cast<long>(docker_cfg_.request_timeout_sec));
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,  curl_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA,      &out.body);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL,        1L);
    curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST,  "DELETE");

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) out.error = curl_easy_strerror(rc);
    else {
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        out.status = status;
    }
    curl_easy_cleanup(h);
    return out;
}

// =============================================================================
//  业务步骤
// =============================================================================
std::string DockerClient::prepare_workdir(const JudgeTask& task, fs::path& out_dir) const {
    out_dir = work_root_ / std::to_string(task.submission_id);
    std::error_code ec;
    // 不 remove_all：dispatcher 负责隔离 workdir（每次 submission_id 不同 → 不同目录）
    // 若目录存在则复用，否则创建。单元测试通过预置 summary.json 来验证读取逻辑。
    fs::create_directories(out_dir / "src", ec);
    if (ec) return "create src dir failed: " + ec.message();
    fs::create_directories(out_dir / "testcases", ec);
    if (ec) return "create testcases dir failed: " + ec.message();
    fs::create_directories(out_dir / "result", ec);
    if (ec) return "create result dir failed: " + ec.message();

    std::string src_filename;
    switch (task.language) {
        case SubmissionLanguage::C:      src_filename = "main.c";    break;
        case SubmissionLanguage::Cpp:    src_filename = "main.cpp";  break;
        case SubmissionLanguage::Java:   src_filename = "Main.java"; break;
        case SubmissionLanguage::Python: src_filename = "main.py";   break;
        case SubmissionLanguage::Go:     src_filename = "main.go";   break;
        default: return "unsupported language";
    }
    if (!write_file(out_dir / "src" / src_filename, task.code))
        return "write source file failed";

    for (std::size_t i = 0; i < task.testcases.size(); ++i) {
        const int idx = static_cast<int>(i + 1);
        if (!write_file(out_dir / "testcases" / (std::to_string(idx) + ".in"),  task.testcases[i].first))
            return "write testcase in failed";
        if (!write_file(out_dir / "testcases" / (std::to_string(idx) + ".out"), task.testcases[i].second))
            return "write testcase out failed";
    }

    json meta;
    meta["language"]        = language_to_string(task.language);
    meta["time_limit_ms"]   = task.limits.time_ms;
    meta["memory_limit_mb"] = task.limits.mem_mb;
    meta["output_limit_mb"] = task.limits.out_mb;
    if (!write_file(out_dir / "meta.json", meta.dump()))
        return "write meta.json failed";
    return {};
}

std::string DockerClient::create_container(const fs::path& workdir,
                                          SubmissionLanguage /*lang*/,
                                          const JudgeLimits& limits,
                                          std::int64_t /*submission_id*/,
                                          const std::string& image,
                                          std::string& out_container_id) const {
    // 沙箱：SPEC §6.4
    constexpr long kMB = 1024L * 1024L;
    long mem_bytes = static_cast<long>(limits.mem_mb) * kMB;

    json body;
    body["Image"]      = image;
    body["Cmd"]        = json::array({"/judge/bin/entrypoint.sh"});
    body["WorkingDir"] = "/judge/work";
    body["User"]       = "judge";
    // 不覆盖 PATH —— 各语言镜像 (judge-java/Go 等) 把工具链放在不同路径
    // (e.g. /opt/java/openjdk/bin, /usr/local/go/bin), Docker 默认 PATH 已包含这些。
    // 强制 GOCACHE 写到 /tmp（tmpfs 64M），绕开 read-only rootfs 限制（Go 编译需要可写缓存目录）
    body["Env"]        = json::array({
        "GOCACHE=/tmp/go-cache",
        "GOPATH=/tmp/go"
    });

    auto& hc = body["HostConfig"];
    hc["AutoRemove"]     = true;
    hc["NetworkMode"]    = "none";
    hc["CapDrop"]        = json::array({"ALL"});
    hc["SecurityOpt"]    = json::array({"no-new-privileges"});
    hc["ReadonlyRootfs"] = true;
    hc["Tmpfs"]          = json::object({{"/tmp", "size=64m,noexec,nosuid,nodev"}});
    hc["Memory"]         = mem_bytes;
    hc["MemorySwap"]     = mem_bytes;        // 禁止 swap
    hc["CpuQuota"]       = 200000;            // 200ms / 1000ms = 0.2 CPU
    hc["CpuPeriod"]      = 100000;
    hc["PidsLimit"]      = 512;   // SPEC §6.4 = 64，但 Go 编译链需 > 100 进程

    // bind mount host_dir → /judge/work
    hc["Mounts"]         = json::array({
        {
            {"Type",   "bind"},
            {"Source", workdir.string()},
            {"Target", "/judge/work"},
        }
    });

    auto r = http_post("/containers/create", body.dump());
    if (!r.error.empty()) return "create: " + r.error;
    if (r.status != 201) {
        return "create: status=" + std::to_string(r.status) + " body=" + truncate(r.body, 300);
    }
    // 响应：{"Id": "...", "Warnings": []}
    try {
        auto j = json::parse(r.body);
        out_container_id = j.value("Id", "");
    } catch (const std::exception& e) {
        return std::string("create: parse Id: ") + e.what();
    }
    if (out_container_id.empty()) return "create: empty Id in response";
    return {};
}

std::string DockerClient::start_container(const std::string& container_id) const {
    auto r = http_post("/containers/" + container_id + "/start", "");
    if (!r.error.empty())  return "start: " + r.error;
    if (r.status != 204 && r.status != 304) {
        return "start: status=" + std::to_string(r.status) + " body=" + truncate(r.body, 200);
    }
    return {};
}

std::string DockerClient::wait_container(const std::string& container_id,
                                       int total_timeout_sec,
                                       int& out_status_code) const {
    out_status_code = -1;
    int sec = std::max(1, total_timeout_sec);
    auto r = http_post("/containers/" + container_id + "/wait?timeout=" + std::to_string(sec), "");
    if (!r.error.empty())  return "wait: " + r.error;
    if (r.status == 404) {
        out_status_code = 0;
        return {};
    }
    if (r.status != 200) {
        return "wait: status=" + std::to_string(r.status) + " body=" + truncate(r.body, 200);
    }
    try {
        auto j = json::parse(r.body);
        if (j.contains("StatusCode")) out_status_code = j["StatusCode"].get<int>();
        return {};
    } catch (const std::exception& e) {
        return std::string("wait: invalid response body: ") + e.what();
    }
}

std::string DockerClient::fetch_logs(const std::string& container_id,
                                    std::string& out_logs) const {
    out_logs.clear();
    auto r = http_get("/containers/" + container_id + "/logs?stdout=1&stderr=1&follow=0");
    if (!r.error.empty()) return "logs: " + r.error;
    if (r.status == 404) return {};
    if (r.status != 200) {
        return "logs: status=" + std::to_string(r.status) + " body=" + truncate(r.body, 200);
    }
    // Docker default logging driver: multiplexed stream with 8-byte header per frame
    //   [stream_type(1) padding(3) size(4)]
    const auto& buf = r.body;
    std::string out;
    std::size_t i = 0;
    while (i + 8 <= buf.size()) {
        std::uint32_t sz = (static_cast<std::uint8_t>(buf[i+4]) << 24) |
                           (static_cast<std::uint8_t>(buf[i+5]) << 16) |
                           (static_cast<std::uint8_t>(buf[i+6]) <<  8) |
                           (static_cast<std::uint8_t>(buf[i+7]));
        i += 8;
        if (i + sz > buf.size()) break;
        out.append(buf.data() + i, sz);
        i += sz;
    }
    out_logs = std::move(out);
    return {};
}

std::string DockerClient::remove_container(const std::string& container_id) const {
    auto r = http_delete("/containers/" + container_id + "?force=true&v=true");
    if (!r.error.empty()) return "remove: " + r.error;
    if (r.status != 204 && r.status != 404) {
        return "remove: status=" + std::to_string(r.status) + " body=" + truncate(r.body, 200);
    }
    return {};
}

std::string DockerClient::read_results(const fs::path& workdir, JudgeResult& out_result) const {
    const fs::path summary_path  = workdir / "result" / "summary.json";
    const fs::path per_case_path = workdir / "result" / "per_case.json";
    const fs::path compile_log   = workdir / "result" / "compile.log";

    if (!fs::exists(summary_path)) {
        out_result.overall = JudgeStatus::SE;
        out_result.result_string = "SE";
        out_result.judge_message = "summary.json not found";
        return {};
    }

    std::string sbody = slurp_file(summary_path);
    if (sbody.empty()) {
        out_result.overall = JudgeStatus::SE;
        out_result.judge_message = "summary.json empty";
        return {};
    }

    json sj;
    try { sj = json::parse(sbody); }
    catch (const std::exception& e) {
        out_result.overall = JudgeStatus::SE;
        out_result.judge_message = std::string("summary.json parse: ") + e.what();
        return {};
    }

    out_result.compile_ok     = sj.value("compile_ok", false);
    out_result.total_score   = sj.value("total_score", 0);
    out_result.time_ms       = sj.value("time_used_ms", 0);
    out_result.mem_kb        = sj.value("memory_used_kb", 0);
    out_result.result_string = sj.value("result", "SE");
    out_result.overall       = status_from_string(out_result.result_string);

    if (sj.contains("compile_log") && sj["compile_log"].is_string()) {
        out_result.compile_output = sj["compile_log"].get<std::string>();
    }
    if (out_result.overall == JudgeStatus::CE && out_result.compile_output.empty()) {
        out_result.compile_output = slurp_file(compile_log);
    }

    if (fs::exists(per_case_path)) {
        std::string pbody = slurp_file(per_case_path);
        if (!pbody.empty()) {
            try {
                auto pj = json::parse(pbody);
                if (pj.contains("cases") && pj["cases"].is_array()) {
                    for (const auto& cj : pj["cases"]) {
                        CaseResult c;
                        c.index     = cj.value("case_index", 0);
                        c.status    = status_from_string(cj.value("status", std::string("SE")));
                        c.time_ms   = cj.value("time_used_ms", 0);
                        c.mem_kb    = cj.value("memory_used_kb", 0);
                        c.score     = cj.value("score", 0);
                        c.is_sample = cj.value("is_sample", false);
                        if (cj.contains("user_output") && cj["user_output"].is_string())
                            c.user_output = cj["user_output"].get<std::string>();
                        if (cj.contains("diff") && cj["diff"].is_string())
                            c.diff_first_line = cj["diff"].get<std::string>();
                        auto exp_path = workdir / "testcases" / (std::to_string(c.index) + ".out");
                        c.expected_output = slurp_file(exp_path);
                        out_result.cases.push_back(std::move(c));
                    }
                }
            } catch (const std::exception& e) {
                out_result.judge_message += "; per_case parse: " + std::string(e.what());
            }
        }
    }
    return {};
}

std::string DockerClient::cleanup_workdir(const fs::path& workdir) const {
    std::error_code ec;
    fs::remove_all(workdir, ec);
    return ec ? "cleanup: " + ec.message() : "";
}

// =============================================================================
//  run() —— SPEC §6.1 完整判题流程
// =============================================================================
JudgeResult DockerClient::run(const JudgeTask& task) const {
    JudgeResult r;
    r.result_string = "SE";
    r.overall = JudgeStatus::SE;

    if (task.image.empty()) {
        r.judge_message = "no image specified";
        return r;
    }

    fs::path workdir;
    auto err = prepare_workdir(task, workdir);
    if (!err.empty()) {
        r.judge_message = truncate(err, 500);
        return r;
    }

    // 1) check image 存在
    if (auto e = check_image(task.image); !e.empty()) {
        r.judge_message = truncate(e, 500);
        cleanup_workdir(workdir);
        return r;
    }

    // 2) create
    std::string container_id;
    err = create_container(workdir, task.language, task.limits, task.submission_id,
                           task.image, container_id);
    if (!err.empty()) {
        r.judge_message = truncate(err, 500);
        cleanup_workdir(workdir);
        return r;
    }

    // 3) start
    if (auto e = start_container(container_id); !e.empty()) {
        r.judge_message = truncate(e, 500);
        remove_container(container_id);
        cleanup_workdir(workdir);
        return r;
    }

    // 4) wait —— 总超时 = 各 case time 之和 + buffer
    //    每点超时累加最多 N * time_limit；这里用 N 倍 time_limit 简化
    const int total_timeout_sec = (task.testcases.empty() ? 1
                                    : (task.limits.time_ms * 1000 * static_cast<int>(task.testcases.size())
                                       + docker_cfg_.container_wait_buffer_sec * 1000) / 1000);
    int exit_code = -1;
    err = wait_container(container_id, total_timeout_sec, exit_code);
    if (!err.empty()) {
        r.judge_message = truncate(err, 500);
        remove_container(container_id);
        cleanup_workdir(workdir);
        return r;
    }

    // 5) 读 results
    err = read_results(workdir, r);
    if (!err.empty()) {
        r.judge_message = truncate(err, 500);
        remove_container(container_id);
        cleanup_workdir(workdir);
        return r;
    }

    // 6) compile log: CE 时拉一次 logs（覆盖 judge 工具的 write_compile_log 异常）
    if (r.overall == JudgeStatus::CE && r.compile_output.empty()) {
        std::string logs;
        if (auto e = fetch_logs(container_id, logs); e.empty() && !logs.empty()) {
            r.compile_output = truncate(logs, 64 * 1024);
        }
    }

    // 7) delete container (AutoRemove=true 通常已删除；force=true 兜底)
    remove_container(container_id);

    // 8) cleanup workdir
    cleanup_workdir(workdir);

    return r;
}

}  // namespace oj::infra
