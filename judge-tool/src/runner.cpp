#include "runner.hpp"
#include "compare.hpp"
#include "monitor.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace judge {

// SIGALRM 处理器 —— 超时后把 child 整个进程组杀掉
// 文件作用域 volatile 变量；处理函数必须 async-signal-safe
namespace {
static volatile pid_t g_alarm_pid    = -1;
static volatile pid_t g_alarm_pgid   = -1;
extern "C" void on_alarm_kill(int /*sig*/) {
    pid_t pg = g_alarm_pgid;
    pid_t p  = g_alarm_pid;
    // 调试输出（async-signal-safe: write 2 是）
    char msg[64];
    int n = snprintf(msg, sizeof(msg), "[ALARM] pg=%d p=%d\n", (int)pg, (int)p);
    if (n > 0) (void)::write(2, msg, (size_t)n);
    if (pg > 0)      ::kill(-pg, SIGKILL);
    else if (p > 0)  ::kill(p, SIGKILL);
}
}  // namespace

namespace fs = std::filesystem;

namespace {

constexpr long KB = 1024L;
constexpr long MB = 1024L * 1024L;

// 读整文件 → string
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// 把 in_path 写到 out_path
bool copy_to(const std::string& in_path, const std::string& out_path) {
    std::ifstream src(in_path, std::ios::binary);
    if (!src) return false;
    std::ofstream dst(out_path, std::ios::binary);
    if (!dst) return false;
    dst << src.rdbuf();
    return dst.good();
}

// 子进程内给编译型语言设 RLIMIT_AS；其他语言跳过
void apply_rlimits_compiled(int mem_mb) {
    struct rlimit rl;
    // RLIMIT_AS 是虚拟地址空间上限
    rl.rlim_cur = (rlim_t)mem_mb * MB;
    rl.rlim_max = (rlim_t)mem_mb * MB;
    ::setrlimit(RLIMIT_AS, &rl);

    // RLIMIT_CPU 是 CPU 秒数（向上取整 +1 缓冲）；细粒度超时由父进程 timer 负责
    rl.rlim_cur = (rlim_t)300;       // 300s hard cap（防 setitimer 失效时兜底）
    rl.rlim_max = (rlim_t)300;
    ::setrlimit(RLIMIT_CPU, &rl);
}

}  // namespace

CaseResult run_case(Language lang,
                    const std::string& run_cmd,
                    const std::string& in_path,
                    const std::string& expected_path,
                    int case_idx,
                    const std::string& out_dir,
                    const Limits& limits) {
    CaseResult c;
    c.index = case_idx;
    c.status = Status::RE;  // 默认 RE（没拿到 AC 之前）

    fs::create_directories(out_dir);

    const std::string out_path  = out_dir + "/run-" + std::to_string(case_idx) + ".out";
    const std::string err_path  = out_dir + "/run-" + std::to_string(case_idx) + ".err";
    const std::string input_fd_path = out_dir + "/run-" + std::to_string(case_idx) + ".in";

    // 把测试点输入 copy 到 out_dir（供 size check 之外的逻辑）
    copy_to(in_path, input_fd_path);

    int in_fd  = ::open(in_path.c_str(),  O_RDONLY);
    int out_fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int err_fd = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (in_fd < 0 || out_fd < 0 || err_fd < 0) {
        if (in_fd  >= 0) ::close(in_fd);
        if (out_fd >= 0) ::close(out_fd);
        if (err_fd >= 0) ::close(err_fd);
        c.status = Status::SE;
        c.stderr_text = "cannot open files";
        return c;
    }

    // SIGALRM 处理器：超时后把 child 整个进程组杀掉
    // 不用 SA_RESTART —— waitpid 收到 EINTR 后循环重试
    struct sigaction sa {};
    sa.sa_handler = on_alarm_kill;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGALRM, &sa, nullptr);

    // 同步管道：child 在 setpgid 后写 1 字节，parent 读到后才开始计时
    int sync_pipe[2];
    if (::pipe(sync_pipe) != 0) {
        c.status = Status::SE;
        c.stderr_text = "pipe() failed";
        return c;
    }

    // 启动 timer（先 setitimer，让 SIGALRM 在 fork 后某时刻可能触发）
    struct itimerval itv {};
    itv.it_value.tv_sec  = limits.time_ms / 1000;
    itv.it_value.tv_usec = (limits.time_ms % 1000) * 1000;
    struct itimerval old_itv {};
    ::setitimer(ITIMER_REAL, &itv, &old_itv);

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(in_fd); ::close(out_fd); ::close(err_fd);
        ::setitimer(ITIMER_REAL, &old_itv, nullptr);
        c.status = Status::SE;
        c.stderr_text = "fork() failed";
        return c;
    }

    auto t0 = std::chrono::steady_clock::now();

    if (pid == 0) {
        // child
        ::setpgid(0, 0);  // 自己的进程组，便于超时后整组 kill
        // 通知 parent：setpgid 已完成
        ::close(sync_pipe[0]);
        char ack = 'g';
        (void)::write(sync_pipe[1], &ack, 1);
        ::close(sync_pipe[1]);

        ::dup2(in_fd,  STDIN_FILENO);
        ::dup2(out_fd, STDOUT_FILENO);
        ::dup2(err_fd, STDERR_FILENO);
        ::close(in_fd); ::close(out_fd); ::close(err_fd);

        // 编译型语言在子进程内设 RLIMIT_AS
        if (lang == Language::C || lang == Language::Cpp || lang == Language::Go) {
            apply_rlimits_compiled(limits.mem_mb);
        }

        // exec via shell -c
        ::execl("/bin/sh", "sh", "-c", run_cmd.c_str(), (char*)nullptr);
        ::_exit(127);
    }

    // parent
    ::close(in_fd); ::close(out_fd); ::close(err_fd);

    // 等 child 写 sync byte —— 保证 child 已 setpgid(0,0) 完成
    ::close(sync_pipe[1]);  // 关闭写端
    char ack;
    ssize_t n = ::read(sync_pipe[0], &ack, 1);
    ::close(sync_pipe[0]);
    if (n != 1) {
        // child 启动失败
        int status;
        ::waitpid(pid, &status, 0);
        c.status = Status::RE;
        c.stderr_text = "child did not ack";
        return c;
    }

    // 此时 child 的 setpgid 已生效，pgid == child pid
    g_alarm_pid  = pid;
    g_alarm_pgid = pid;

    // 启动内存监控
    MemoryMonitor mon(pid);
    mon.start();

    int status = 0;
    bool   killed_by_timer = false;
    bool   killed_by_ole    = false;
    pid_t  r = 0;

    // WNOHANG 轮询 + EINTR 重试；上限 1s（远超合理 time_limit）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) break;           // 正常收尸
        if (r < 0 && errno == EINTR) continue;  // signal 重试
        if (r == 0) {
            // child 还活着；如果是 SIGALRM 触发的，等待 handler 杀子
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        break;  // 其他错误
    }
    if (r != pid) {
        // 兜底：超时兜不住就强杀
        ::kill(-g_alarm_pgid, SIGKILL);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        killed_by_timer = true;
    }

    auto t1 = std::chrono::steady_clock::now();
    mon.stop();

    // 关掉 timer + 清 signal handler
    struct itimerval disable {};
    ::setitimer(ITIMER_REAL, &disable, nullptr);
    sa.sa_handler = SIG_DFL;
    ::sigaction(SIGALRM, &sa, nullptr);
    g_alarm_pid = g_alarm_pgid = -1;

    int elapsed_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // OLE 检查：只要输出文件超过上限就标 OLE（不论程序是否被信号杀）
    long out_size = 0;
    try {
        out_size = (long)fs::file_size(out_path);
    } catch (...) { out_size = 0; }
    if (out_size > (long)limits.out_mb * MB) {
        c.status = Status::OLE;
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        return c;
    }

    // 检查是否被 timer (SIGALRM) 杀掉
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
        killed_by_timer = true;
    }

    // 1) OLE
    if (killed_by_ole) {
        c.status = Status::OLE;
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        return c;
    }

    // 2) TLE
    if (killed_by_timer || elapsed_ms > limits.time_ms) {
        c.status = Status::TLE;
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        return c;
    }

    // 3) MLE —— child 被 SIGKILL，但 OLE/TLE 不命中
    if (WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) {
        c.status = Status::MLE;
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        return c;
    }

    // 4) 其它信号（SIGSEGV / SIGABRT / ...）→ RE
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        c.stderr_text = "killed by signal " + std::to_string(sig);
        // RLIMIT_AS / OOM killer 对编译型语言有时发 SIGBUS/SIGSEGV；
        // 当 mem 监控看到内存曾增长、且 child 显然在做内存分配时，按 MLE 算。
        if ((lang == Language::C || lang == Language::Cpp || lang == Language::Go) &&
            (sig == SIGBUS || sig == SIGSEGV || sig == SIGKILL) &&
            mon.peak_kb() > 0) {
            c.status = Status::MLE;
        } else {
            c.status = Status::RE;
        }
        return c;
    }

    // 5) 退出码非 0 → RE
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        c.status = Status::RE;
        c.time_ms = elapsed_ms;
        c.mem_kb  = mon.peak_kb();
        c.stderr_text = "exit " + std::to_string(WEXITSTATUS(status));
        c.stderr_text += "\n" + slurp(err_path);
        return c;
    }

    // 6) 退出 0 → 比对输出
    c.time_ms = elapsed_ms;
    c.mem_kb  = mon.peak_kb();
    c.user_output = slurp(out_path);
    c.stderr_text = slurp(err_path);

    std::string expected = slurp(expected_path);
    std::string diff;
    if (compare_outputs(expected, c.user_output, &diff)) {
        c.status = Status::AC;
    } else {
        c.status = Status::WA;
        c.diff_first_line = diff;
    }
    return c;
}

}  // namespace judge
