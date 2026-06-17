#include "result.hpp"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace judge {

using nlohmann::json;

void write_per_case_json(const std::string& path,
                         const Limits& limits,
                         const std::vector<CaseResult>& cases) {
    json j;
    j["language"]        = language_id(limits.language);
    j["time_limit_ms"]   = limits.time_ms;
    j["memory_limit_mb"] = limits.mem_mb;
    j["output_limit_mb"] = limits.out_mb;
    j["cases"] = json::array();
    for (const auto& c : cases) {
        json jc;
        jc["case_index"]     = c.index;
        jc["status"]         = to_string(c.status);
        jc["time_used_ms"]   = c.time_ms;
        jc["memory_used_kb"] = c.mem_kb;
        // 仅 WA / RE 时回填 diff 第一行，方便定位
        if (c.status == Status::WA && !c.diff_first_line.empty()) {
            jc["diff"] = c.diff_first_line;
        }
        // user_output 始终写入；host 决定是否落库（仅样例点）
        jc["user_output"] = c.user_output;
        if (!c.stderr_text.empty()) {
            jc["stderr"] = c.stderr_text;
        }
        j["cases"].push_back(jc);
    }
    std::ofstream f(path);
    f << j.dump(2);
}

void write_summary_json(const std::string& path, const Summary& sum) {
    json j;
    j["compile_ok"]   = sum.compile_ok;
    j["result"]       = to_string(sum.result);
    j["total_score"]  = sum.total_score;       // judge 不算分；host 算
    j["time_used_ms"] = sum.time_ms;
    j["memory_used_kb"] = sum.mem_kb;
    if (!sum.compile_log.empty()) j["compile_log"] = sum.compile_log;
    std::ofstream f(path);
    f << j.dump(2);
}

void write_compile_log(const std::string& path, const std::string& text) {
    std::ofstream f(path);
    f << text;
}

Summary aggregate(const Limits& limits,
                  bool compile_ok,
                  const std::string& compile_log_text,
                  std::vector<CaseResult> cases) {
    Summary s;
    s.compile_ok  = compile_ok;
    s.compile_log = compile_log_text;
    s.cases       = std::move(cases);

    if (!compile_ok) {
        s.result = Status::CE;
        s.time_ms = 0;
        s.mem_kb  = 0;
        return s;
    }

    // 编译 OK 但 0 case —— 视为 SE
    if (s.cases.empty()) {
        s.result = Status::SE;
        return s;
    }

    // 取 max severity
    Status worst = Status::AC;
    int worst_t = 0;
    long worst_m = 0;
    for (const auto& c : s.cases) {
        if (severity(c.status) > severity(worst)) worst = c.status;
        worst_t = std::max(worst_t, c.time_ms);
        worst_m = std::max(worst_m, c.mem_kb);
    }
    s.result  = worst;
    s.time_ms = worst_t;
    s.mem_kb  = worst_m;
    (void)limits;
    return s;
}

}  // namespace judge
