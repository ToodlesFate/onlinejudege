#pragma once
#include "judge/types.hpp"
#include <string>

namespace judge {

// 写 per_case.json —— SPEC §6.1 result/per_case.json
//   { "language": "cpp", "time_limit_ms": 2000, ...,
//     "cases": [ { "case_index": 1, "status": "AC", "time_used_ms": 15, "memory_used_kb": 4096, "user_output": "...", "diff": "..." } ] }
void write_per_case_json(const std::string& path,
                         const Limits& limits,
                         const std::vector<CaseResult>& cases);

// 写 summary.json —— SPEC §6.1 result/summary.json
//   { "compile_ok": true, "result": "AC", "total_score": 0, "time_used_ms": 15, "memory_used_kb": 4096 }
void write_summary_json(const std::string& path, const Summary& sum);

// 写 compile.log（CE 时）
void write_compile_log(const std::string& path, const std::string& text);

// 把 cases 聚合成整体 status + time/mem
Summary aggregate(const Limits& limits,
                  bool compile_ok,
                  const std::string& compile_log_text,
                  std::vector<CaseResult> cases);

}  // namespace judge
