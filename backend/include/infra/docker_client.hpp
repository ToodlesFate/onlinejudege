#pragma once

// =============================================================================
//  oj::infra::DockerClient —— Docker Engine API 客户端 (libcurl)
//  SPEC §3.2.2 "DockerClient" / §6.1 / §6.4
//
//  职责：
//    1) 调用 Docker Engine API（HTTP over Unix socket 或 TCP）：
//         - POST /containers/create  (image, Cmd, HostConfig=沙箱, Mounts)
//         - POST /containers/{id}/start
//         - POST /containers/{id}/wait
//         - GET  /containers/{id}/logs?stdout=1&stderr=1
//         - DELETE /containers/{id}?force=true
//    2) 准备 /tmp/oj/<id>/ 工作目录：
//         - src/<file>     (源代码；按语言写为 main.cpp / Main.java / main.py / main.go)
//         - testcases/<N>.in / <N>.out
//         - meta.json
//    3) 收集结果：
//         - 读 result/summary.json + per_case.json
//         - 编译失败时（compile_ok=false）从 result/compile.log 拿 stdout/stderr
//
//  设计要点：
//    - libcurl 全局 init/cleanup 由 RAII 风格静态对象托管（线程安全）
//    - 单次 HTTP 调用可注入超时；wait 单独用更长 timeout
//    - 出错时记录 HTTP 状态码 + 响应体到 judge_message（限制 500 字符）
//    - 任何阶段失败都返回 Status=SE，不抛异常
// =============================================================================

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/config.hpp"

namespace oj::infra {

// 8 态判定（与 submissions.result / submission_cases.status 枚举对齐）
enum class JudgeStatus {
    AC,   // Accepted
    WA,   // Wrong Answer
    TLE,  // Time Limit Exceeded
    MLE,  // Memory Limit Exceeded
    OLE,  // Output Limit Exceeded
    RE,   // Runtime Error
    CE,   // Compile Error
    SE,   // System Error
};

const char* to_string(JudgeStatus s) noexcept;
JudgeStatus status_from_string(std::string_view s) noexcept;

// 5 种提交语言（与 submissions.language 枚举对齐）
enum class SubmissionLanguage {
    Unknown = 0,
    C,
    Cpp,
    Java,
    Python,
    Go,
};

SubmissionLanguage language_from_string(std::string_view s) noexcept;
const char*         language_to_string(SubmissionLanguage l) noexcept;

// 资源限制
struct JudgeLimits {
    int time_ms   = 2000;
    int mem_mb    = 256;
    int out_mb    = 64;
};

// 单测试点结果（对应 submission_cases）
struct CaseResult {
    int            index     = 0;
    JudgeStatus    status    = JudgeStatus::SE;
    int            time_ms   = 0;
    long           mem_kb    = 0;
    int            score     = 0;
    bool           is_sample = false;
    std::string    user_output;     // 容器内 stdout；host 决定是否持久化
    std::string    expected_output; // 用于 diff
    std::string    diff_first_line; // WA 时给一行 diff
};

// 整道题结果（对应 submissions）
struct JudgeResult {
    JudgeStatus             overall = JudgeStatus::SE;
    std::string             result_string;   // 8 态字符串（与 SPEC §5.3 对齐）
    bool                    compile_ok   = false;
    std::string             compile_output;  // CE 时填
    int                     total_score  = 0;
    int                     time_ms      = 0;   // 整道题总耗时（最慢点）
    long                    mem_kb       = 0;   // 整道题总内存（峰值点）
    std::vector<CaseResult> cases;
    std::string             judge_message;     // SE 时填（≤ 500 字符）
};

// 单次判题任务：包含 language + 源代码 + 测试点 + 限制 + image
struct JudgeTask {
    std::int64_t        submission_id = 0;
    SubmissionLanguage  language      = SubmissionLanguage::Cpp;
    std::string         code;                       // 源代码
    std::vector<std::pair<std::string, std::string>>  testcases;  // (input, expected_output)
    JudgeLimits         limits;
    std::string         image;                      // judge image 名（如 "judge-cpp:1.0"）
};

// 主类
class DockerClient {
public:
    explicit DockerClient(common::DockerConfig cfg);
    ~DockerClient();

    DockerClient(const DockerClient&)            = delete;
    DockerClient& operator=(const DockerClient&) = delete;

    /**
     * 检查 Docker daemon 是否可达（轻量 HEAD /_ping）。
     * 返回空字符串 = OK；非空 = 错误信息。
     */
    [[nodiscard]] std::string ping() const;

    /**
     * 验证 image 是否已存在于本地。
     * 返回空 = OK；非空 = 错误信息（含 pull 建议）。
     */
    [[nodiscard]] std::string check_image(std::string_view image) const;

    /**
     * 整道题完整判题流程 —— SPEC §6.1：
     *   1) 准备 /tmp/oj/<id>/{src,testcases,meta.json,result/}
     *   2) POST /containers/create
     *   3) POST /containers/{id}/start
     *   4) POST /containers/{id}/wait
     *   5) GET  /containers/{id}/logs（仅 CE 拿 stdout/stderr）
     *   6) DELETE /containers/{id}?force=true
     *   7) 读 result/summary.json + per_case.json
     *
     * 任何阶段失败 → 返回 SE 结果 + judge_message。
     */
    [[nodiscard]] JudgeResult run(const JudgeTask& task) const;

    // 测试用：注入 work_root（默认 cfg 路径或本构造）
    [[nodiscard]] const std::filesystem::path& work_root() const noexcept { return work_root_; }
    void set_work_root(std::filesystem::path p) { work_root_ = std::move(p); }

    // 测试用：注入镜像名覆盖（默认从 cfg.images 选）
    [[nodiscard]] std::string image_for(SubmissionLanguage l) const;

private:
    common::DockerConfig docker_cfg_;
    std::filesystem::path work_root_;

    // ---- 内部 HTTP 调用 ----
    struct HttpResponse {
        long status = 0;            // HTTP status code
        std::string body;            // 响应体
        std::string error;           // 网络 / libcurl 错误
    };

    [[nodiscard]] HttpResponse http_get(std::string_view path) const;
    [[nodiscard]] HttpResponse http_post(std::string_view path, std::string_view body,
                                        std::string_view content_type = "application/json") const;
    [[nodiscard]] HttpResponse http_delete(std::string_view path) const;

    // ---- 业务步骤 ----
    [[nodiscard]] std::string prepare_workdir(const JudgeTask& task,
                                              std::filesystem::path& out_dir) const;
    [[nodiscard]] std::string create_container(const std::filesystem::path& workdir,
                                               SubmissionLanguage lang,
                                               const JudgeLimits& limits,
                                               std::int64_t submission_id,
                                               const std::string& image,
                                               std::string& out_container_id) const;
    [[nodiscard]] std::string start_container(const std::string& container_id) const;
    [[nodiscard]] std::string wait_container(const std::string& container_id,
                                            int total_timeout_sec,
                                            int& out_status_code) const;
    [[nodiscard]] std::string fetch_logs(const std::string& container_id,
                                         std::string& out_logs) const;
    [[nodiscard]] std::string remove_container(const std::string& container_id) const;
    [[nodiscard]] std::string read_results(const std::filesystem::path& workdir,
                                          JudgeResult& out_result) const;
    [[nodiscard]] std::string cleanup_workdir(const std::filesystem::path& workdir) const;
};

}  // namespace oj::infra
