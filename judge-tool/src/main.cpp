// =============================================================================
//  judge/main.cpp —— 判题工具入口 (SPEC §6.1)
//
//  用法：
//    /judge/bin/judge --meta meta.json --src src/ --tests testcases/ --out result/
//
//  流程：
//    1) 读 meta.json → Limits
//    2) compile() → CE? → 写 compile.log + summary.json → 退出 10
//    3) 扫 tests/ 找 1.in, 2.in, ...  → 每个点 run_case()
//    4) aggregate() → 写 per_case.json + summary.json → 退出 0
//
//  退出码：
//    0  = 跑完所有点（可能非 AC）
//    10 = 编译错误
//    20 = 系统错误（meta 缺失 / src 不存在 / 没测试点）
// =============================================================================

#include "compile.hpp"
#include "meta.hpp"
#include "result.hpp"
#include "runner.hpp"
#include "judge/types.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Args {
    std::string meta_path;
    std::string src_dir;
    std::string tests_dir;
    std::string out_dir;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto need_val = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(20);
            }
            return argv[++i];
        };
        if      (k == "--meta")   a.meta_path  = need_val("--meta");
        else if (k == "--src")    a.src_dir    = need_val("--src");
        else if (k == "--tests")  a.tests_dir  = need_val("--tests");
        else if (k == "--out")    a.out_dir    = need_val("--out");
        else if (k == "-h" || k == "--help") {
            std::printf("usage: judge --meta meta.json --src src/ --tests testcases/ --out result/\n");
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", k.c_str());
            std::exit(20);
        }
    }
    if (a.meta_path.empty() || a.src_dir.empty() ||
        a.tests_dir.empty() || a.out_dir.empty()) {
        std::fprintf(stderr, "missing required arg(s)\n");
        std::exit(20);
    }
    return a;
}

// 跑具体语言测试点的命令模板
std::string make_run_cmd(judge::Language lang,
                         const std::string& working_dir,
                         int mem_mb) {
    (void)working_dir;  // 命令里走相对路径 bin/, 假设已经 chdir 到 src_dir
    switch (lang) {
        case judge::Language::C:
        case judge::Language::Cpp:
        case judge::Language::Go:
            return "./bin";
        case judge::Language::Java:
            // SPEC §6.2 Java: java -Xss64m -Xmx<题目内存>M Main
            return "java -Xss64m -Xmx" + std::to_string(mem_mb) + "M -cp bin Main";
        case judge::Language::Python:
            return "python3 main.py";
        default:
            return "false";
    }
}

// 扫描 tests_dir，找 1.in, 2.in, ... 返回 1-based idx 列表
std::vector<int> discover_case_indices(const std::string& tests_dir) {
    std::vector<int> out;
    if (!fs::exists(tests_dir)) return out;
    for (const auto& entry : fs::directory_iterator(tests_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        // 形如 "1.in" / "1.out" —— 取 .in
        auto pos = name.rfind(".in");
        if (pos == std::string::npos) continue;
        if (pos == 0) continue;
        try {
            int n = std::stoi(name.substr(0, pos));
            out.push_back(n);
        } catch (...) {}
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    fs::create_directories(a.out_dir);

    // 1) meta.json
    std::string err;
    auto limits_opt = judge::read_meta_file(a.meta_path, &err);
    if (!limits_opt) {
        std::fprintf(stderr, "judge: %s\n", err.c_str());
        // 写 SE summary
        judge::Summary s;
        s.compile_ok = false;
        s.compile_log = err;
        s.result = judge::Status::SE;
        judge::write_summary_json(a.out_dir + "/summary.json", s);
        return 20;
    }
    judge::Limits limits = *limits_opt;

    // chdir 到 src_dir —— 编译产物 ./bin 与运行时命令的相对路径都依赖此 cwd
    if (::chdir(a.src_dir.c_str()) != 0) {
        std::fprintf(stderr, "judge: chdir(%s) failed: %s\n",
                     a.src_dir.c_str(), std::strerror(errno));
        return 20;
    }

    // 2) compile
    auto cr = judge::compile(limits.language, a.src_dir, a.src_dir, limits.mem_mb);
    if (!cr.ok) {
        std::string log = cr.stdout_text + cr.stderr_text;
        judge::write_compile_log(a.out_dir + "/compile.log", log);
        judge::Summary s;
        s.compile_ok  = false;
        s.compile_log = log;
        s.result      = judge::Status::CE;
        judge::write_summary_json(a.out_dir + "/summary.json", s);
        // per_case.json 在 CE 时也写一个空 cases 的（host 容易处理）
        judge::write_per_case_json(a.out_dir + "/per_case.json", limits, {});
        return 10;
    }

    // 3) per-case run
    auto case_idxs = discover_case_indices(a.tests_dir);
    if (case_idxs.empty()) {
        std::fprintf(stderr, "judge: no test cases found in %s\n", a.tests_dir.c_str());
        judge::Summary s;
        s.compile_ok = true;
        s.result     = judge::Status::SE;
        judge::write_summary_json(a.out_dir + "/summary.json", s);
        judge::write_per_case_json(a.out_dir + "/per_case.json", limits, {});
        return 20;
    }

    std::string run_cmd = make_run_cmd(limits.language, a.src_dir, limits.mem_mb);
    std::vector<judge::CaseResult> cases;
    cases.reserve(case_idxs.size());

    for (int idx : case_idxs) {
        std::string in_path  = a.tests_dir + "/" + std::to_string(idx) + ".in";
        std::string out_path = a.tests_dir + "/" + std::to_string(idx) + ".out";
        if (!fs::exists(in_path) || !fs::exists(out_path)) continue;
        // 早期终止：任一 case 为 SE 后继续跑（host 仍要看其它点状态）
        auto c = judge::run_case(limits.language, run_cmd,
                                 in_path, out_path, idx,
                                 a.out_dir, limits);
        cases.push_back(c);
    }

    // 4) aggregate + write
    judge::Summary sum = judge::aggregate(limits, /*compile_ok=*/true,
                                          /*compile_log=*/"", std::move(cases));
    judge::write_per_case_json(a.out_dir + "/per_case.json", limits, sum.cases);
    judge::write_summary_json(a.out_dir + "/summary.json", sum);

    return 0;
}
