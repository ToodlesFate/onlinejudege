// =============================================================================
//  judge/types.hpp —— 公共类型 (SPEC §6.1)
// =============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace judge {

// 5 种语言 (SPEC §6.2)
enum class Language {
    Unknown = 0,
    C,
    Cpp,
    Java,
    Python,
    Go,
};

// 8 态结果 (SPEC §2.3.2)
enum class Status {
    AC,   // Accepted
    WA,   // Wrong Answer
    TLE,  // Time Limit Exceeded
    MLE,  // Memory Limit Exceeded
    OLE,  // Output Limit Exceeded
    RE,   // Runtime Error
    CE,   // Compile Error
    SE,   // System Error
};

const char* to_string(Status s) noexcept;
Status      status_from_string(const std::string& s) noexcept;

// 把 Language 转成后端 submissions.language 枚举字符串 ("c" / "cpp" / "java" / "python" / "go")
const char* language_id(Language l) noexcept;

// 严重度（用于 overall 聚合，越大越坏）
int severity(Status s) noexcept;

// 限制 + 语言
struct Limits {
    Language language  = Language::Unknown;
    int      time_ms   = 2000;     // 单测点 wall-clock 上限
    int      mem_mb    = 256;      // 编译语种用 prlimit AS 强制；解释语种靠容器
    int      out_mb    = 64;       // stdout 大小上限
};

// 单测点结果 (SPEC §4.2 submission_cases)
struct CaseResult {
    int          index     = 0;    // 1-based
    Status       status    = Status::SE;
    int          time_ms   = 0;    // wall-clock
    long         mem_kb    = 0;    // peak RSS / VmHWM
    std::string  user_output;      // 用于样例点（host 决定是否回填 DB）
    std::string  stderr_text;      // RE 时记录
    std::string  diff_first_line;  // WA 时给一行 diff
};

// 整道题 summary
struct Summary {
    bool              compile_ok = false;
    std::string       compile_log;
    Status            result     = Status::SE;
    int               total_score = 0;     // judge 不算分；host 算
    int               time_ms    = 0;      // 整道题总耗时 = max(各点)
    long              mem_kb     = 0;      // 整道题总内存 = max(各点)
    std::vector<CaseResult> cases;
};

}  // namespace judge
