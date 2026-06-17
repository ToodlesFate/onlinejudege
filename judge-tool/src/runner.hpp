#pragma once
#include "judge/types.hpp"
#include <string>

namespace judge {

// 跑单个测试点 —— SPEC §6.1 步骤 2
//   in_path:    测试点输入（如 testcases/1.in）
//   expected_path: 期望输出（如 testcases/1.out）
//   case_idx:   1-based case_index
//   out_dir:    临时输出目录 —— 写 run-N.out, run-N.err
//
//  行为：
//   - 编译型语言 (C/C++/Go) 子进程内 prlimit RLIMIT_AS 到 mem_mb
//   - 时间用 setitimer (real-time) 监控，超时 kill(SIGKILL) → TLE
//   - 父进程后台线程采 /proc/<pid>/status 取峰值 RSS → 写入 case.mem_kb
//   - 输出超过 out_mb → 立刻 SIGKILL → OLE
//   - 退出码非 0 且非被信号杀 → RE
//   - 写完后用 compare_outputs(expected, actual) 比对 → AC / WA
CaseResult run_case(Language lang,
                    const std::string& run_cmd,    // 实际跑的命令（编译器产物 + 参数）
                    const std::string& in_path,
                    const std::string& expected_path,
                    int case_idx,
                    const std::string& out_dir,     // 临时输出 run-N.out, run-N.err
                    const Limits& limits);

}  // namespace judge
