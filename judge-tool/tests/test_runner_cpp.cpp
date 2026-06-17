// =============================================================================
//  tests/test_runner_cpp.cpp —— 真实 C++ 编译 + 运行测试点
//
//  这些测试调用编译后的 `judge` 二进制，构造真实的 src/ + tests/ + out/ 工作目录。
//  覆盖 SPEC §6.1 全部 8 态（除 MLE 和 OLE，这两个需要特殊环境，可能略过）。
//
//  注意：测试假定 g++ / python3 在 PATH 中。
// =============================================================================
#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

class JudgeRunnerFixture : public ::testing::Test {
protected:
    fs::path work_;

    void SetUp() override {
        char tpl[] = "/tmp/oj_judge_XXXXXX";
        char* dir = mkdtemp(tpl);
        ASSERT_NE(dir, nullptr);
        work_ = dir;
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(work_, ec);
    }

    fs::path src_dir()     { return work_ / "src"; }
    fs::path tests_dir()   { return work_ / "testcases"; }
    fs::path out_dir()     { return work_ / "result"; }
    fs::path meta_path()   { return work_ / "meta.json"; }

    void write_meta(const std::string& lang, int time_ms = 2000, int mem_mb = 256, int out_mb = 64) {
        std::ofstream f(meta_path());
        f << R"({"language":")" << lang
          << R"(","time_limit_ms":)" << time_ms
          << R"(,"memory_limit_mb":)" << mem_mb
          << R"(,"output_limit_mb":)" << out_mb
          << "}";
    }
    void write_src(const std::string& name, const std::string& content) {
        fs::create_directories(src_dir());
        std::ofstream f(src_dir() / name);
        f << content;
    }
    void write_case(int idx, const std::string& in_text, const std::string& out_text) {
        fs::create_directories(tests_dir());
        std::ofstream fi(tests_dir() / (std::to_string(idx) + ".in"));
        fi << in_text;
        std::ofstream fo(tests_dir() / (std::to_string(idx) + ".out"));
        fo << out_text;
    }
    void write_case(int idx, const std::string& out_text) {
        write_case(idx, "", out_text);
    }

    struct RunResult {
        int   exit_code = -1;
        std::string summary;
    };

    RunResult run_judge() {
        // 找 judge 二进制 —— 优先用同目录（开发环境），fallback 到 install/bin
        fs::path bin = fs::path(JUDGE_BIN_PATH);
        if (!fs::exists(bin)) {
            ADD_FAILURE() << "judge binary not found: " << bin;
            return {-1, ""};
        }

        fs::create_directories(out_dir());

        std::string cmd = bin.string() +
            " --meta "   + meta_path().string() +
            " --src "    + src_dir().string() +
            " --tests "  + tests_dir().string() +
            " --out "    + out_dir().string();

        // chdir 到 src_dir 以满足 g++ 写 ./bin
        std::string full = "(cd " + src_dir().string() + " && " + cmd + ")";
        FILE* p = ::popen(full.c_str(), "r");
        if (!p) return {-1, ""};
        char buf[4096];
        std::string out;
        while (fgets(buf, sizeof(buf), p)) out += buf;
        int rc = ::pclose(p);
        if (WIFEXITED(rc)) rc = WEXITSTATUS(rc);
        // Also dump per_case for debugging
        return {rc, out};
    }

    std::string read_summary() {
        std::ifstream f(out_dir() / "summary.json");
        if (!f) return "";
        std::ostringstream ss; ss << f.rdbuf();
        return ss.str();
    }
    std::string read_per_case() {
        std::ifstream f(out_dir() / "per_case.json");
        if (!f) return "";
        std::ostringstream ss; ss << f.rdbuf();
        return ss.str();
    }
    std::string read_compile_log() {
        std::ifstream f(out_dir() / "compile.log");
        if (!f) return "";
        std::ostringstream ss; ss << f.rdbuf();
        return ss.str();
    }
};

// ----------------------------------------------------------------------------
//  AC
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_AC) {
    write_meta("cpp");
    write_src("main.cpp", R"(
#include <cstdio>
int main() {
    int a, b; scanf("%d%d", &a, &b);
    printf("%d\n", a + b);
    return 0;
}
)");
    write_case(1, "2 3\n", "5\n");
    write_case(2, "10 20\n", "30\n");

    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0) << r.summary;
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"AC\""), std::string::npos) << s;
    auto p = read_per_case();
    EXPECT_NE(p.find("\"status\": \"AC\""), std::string::npos) << p;
    // 总耗时 = max
    EXPECT_NE(p.find("\"time_used_ms\""), std::string::npos);
}

// ----------------------------------------------------------------------------
//  WA
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_WA) {
    write_meta("cpp");
    write_src("main.cpp", R"(
#include <cstdio>
int main() {
    int a, b; scanf("%d%d", &a, &b);
    printf("%d\n", a - b);  // 故意写错
    return 0;
}
)");
    write_case(1, "2 3\n", "5\n");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0);
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"WA\""), std::string::npos) << s;
}

// ----------------------------------------------------------------------------
//  CE
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_CE) {
    write_meta("cpp");
    write_src("main.cpp", R"(
int main( {
    return 0;
}
)");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 10);  // SPEC §6.1: 编译失败退出 10
    auto s = read_summary();
    EXPECT_NE(s.find("\"compile_ok\": false"), std::string::npos) << s;
    EXPECT_NE(s.find("\"result\": \"CE\""), std::string::npos) << s;
    EXPECT_FALSE(read_compile_log().empty());
}

// ----------------------------------------------------------------------------
//  TLE —— while(1) 死循环
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_TLE) {
    write_meta("cpp", /*time_ms=*/500);
    write_src("main.cpp", R"(
#include <cstdio>
int main() {
    while (1) {}
    return 0;
}
)");
    write_case(1, "", "");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0);
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"TLE\""), std::string::npos) << s;
    // TLE 的 time_used_ms 应大致 >= time_limit
    auto p = read_per_case();
    EXPECT_NE(p.find("\"status\": \"TLE\""), std::string::npos) << p;
}

// ----------------------------------------------------------------------------
//  RE —— 退出码非 0
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_RE) {
    write_meta("cpp");
    write_src("main.cpp", R"(
#include <cstdlib>
int main() { return 1; }
)");
    write_case(1, "", "");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0);
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"RE\""), std::string::npos) << s;
}

// ----------------------------------------------------------------------------
//  C 同样能 AC
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, C_AC) {
    write_meta("c");
    write_src("main.c", R"(
#include <stdio.h>
int main(void) {
    printf("hi\n");
    return 0;
}
)");
    write_case(1, "", "hi\n");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0) << r.summary;
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"AC\""), std::string::npos) << s;
}

// ----------------------------------------------------------------------------
//  Python AC / WA / TLE
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Python_AC) {
    write_meta("python", /*time_ms=*/2000);
    write_src("main.py", R"(
import sys
for line in sys.stdin:
    a, b = map(int, line.split())
    print(a + b)
)");
    write_case(1, "2 3\n", "5\n");
    auto r = run_judge();
    EXPECT_EQ(r.exit_code, 0) << r.summary;
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"AC\""), std::string::npos) << s;
}

TEST_F(JudgeRunnerFixture, Python_WA) {
    write_meta("python");
    write_src("main.py", R"(
import sys
for line in sys.stdin:
    a, b = map(int, line.split())
    print(a - b)
)");
    write_case(1, "2 3\n", "5\n");
    auto r = run_judge();
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"WA\""), std::string::npos) << s;
}

TEST_F(JudgeRunnerFixture, Python_TLE) {
    write_meta("python", /*time_ms=*/500);
    write_src("main.py", R"(
while True:
    pass
)");
    write_case(1, "", "");
    auto r = run_judge();
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"TLE\""), std::string::npos) << s;
}

// ----------------------------------------------------------------------------
//  MLE —— 在没有 cgroup 限制时，RLIMIT_AS 让 mmap 直接返回 MAP_FAILED；
//  真正的 MLE 触发需要容器 --memory=（生产环境由 DockerClient 配置）。
//  这里退一步：用 mmap 超额 + 写第一字节的最小程序，验证 runner 不会误判为 AC。
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_MLE_Overcommit) {
    // mem_mb=32, mmap 64MB → RLIMIT_AS 立即拒绝 → 程序返回 1
    // 期望：runner 把这归为 RE（程序错误退出），不是 AC。
    // 真正的 MLE 路径通过 cgroup 限制验证（见 test_mle_classification）
    write_meta("cpp", /*time_ms=*/2000, /*mem_mb=*/32);
    write_src("main.cpp", R"(
#include <sys/mman.h>
#include <cstddef>
int main() {
    constexpr long SZ = 64L * 1024 * 1024;
    void* p = ::mmap(nullptr, SZ, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 1;
    static_cast<volatile char*>(p)[0] = 'x';
    return 0;
}
)");
    write_case(1, "", "");
    auto r = run_judge();
    auto s = read_summary();
    // 程序因 mmap 失败退出 1 → runner 报 RE（不是 AC）
    EXPECT_NE(s.find("\"result\": \"RE\""), std::string::npos) << s;
}

// ----------------------------------------------------------------------------
//  OLE —— 输出超限
// ----------------------------------------------------------------------------
TEST_F(JudgeRunnerFixture, Cpp_OLE) {
    // out_mb=4 → 输出 8 MB
    write_meta("cpp", /*time_ms=*/2000, /*mem_mb=*/256, /*out_mb=*/4);
    write_src("main.cpp", R"(
#include <cstdio>
int main() {
    // 输出 8 MB（每行 1024 字节 × 8192 行）
    char buf[1024];
    for (int i = 0; i < 1024; ++i) buf[i] = 'x';
    for (int i = 0; i < 8192; ++i) {
        fwrite(buf, 1, 1024, stdout);
    }
    return 0;
}
)");
    write_case(1, "", "");
    auto r = run_judge();
    auto s = read_summary();
    EXPECT_NE(s.find("\"result\": \"OLE\""), std::string::npos) << s;
}

}  // namespace
