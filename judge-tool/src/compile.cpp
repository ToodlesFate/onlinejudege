#include "compile.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <sys/wait.h>

namespace judge {

namespace fs = std::filesystem;

namespace {

// 在工作目录中跑一条 shell 命令，捕获 stdout/stderr。
//   - cmd: 在 shell (-c) 中执行的字符串
//   - workdir: chdir 到这里
CompileResult run_shell(const std::string& cmd, const std::string& workdir) {
    CompileResult r;

    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        r.stderr_text = "pipe() failed";
        return r;
    }

    pid_t pid = fork();
    if (pid < 0) {
        r.stderr_text = "fork() failed";
        return r;
    }

    if (pid == 0) {
        // child
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        ::close(out_pipe[1]);
        ::close(err_pipe[1]);
        if (!workdir.empty()) ::chdir(workdir.c_str());
        // /bin/sh -c <cmd>
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        ::_exit(127);
    }

    // parent
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    auto drain = [](int fd) {
        std::string out;
        char buf[4096];
        while (true) {
            ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            out.append(buf, buf + n);
        }
        return out;
    };
    r.stdout_text = drain(out_pipe[0]);
    r.stderr_text = drain(err_pipe[0]);
    ::close(out_pipe[0]);
    ::close(err_pipe[0]);

    int status = 0;
    if (::waitpid(pid, &status, 0) < 0) {
        r.stderr_text += "\nwaitpid() failed";
        return r;
    }
    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
        r.ok = (r.exit_code == 0);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);
        r.ok = false;
        r.stderr_text += "\n[killed by signal " + std::to_string(WTERMSIG(status)) + "]";
    }
    return r;
}

}  // namespace

std::string make_build_script(Language lang,
                              const std::string& /*src_dir*/,
                              const std::string& /*working_dir*/,
                              int mem_mb) {
    std::ostringstream cmd;
    // 注意：judge 进程内部已经 chdir 到 src_dir（main.cpp 中）；
    //       所以源文件直接是 main.cpp / Main.java / main.py / main.go，没有 src/ 前缀。
    switch (lang) {
        case Language::C:
            // SPEC §6.2 C: gcc -O2 -std=c11 -o bin main.c -lm
            cmd << "gcc -O2 -std=c11 -o bin main.c -lm 2>&1";
            break;
        case Language::Cpp:
            // SPEC §6.2 C++: g++ -O2 -std=c++17 -o bin main.cpp
            cmd << "g++ -O2 -std=c++17 -o bin main.cpp 2>&1";
            break;
        case Language::Java:
            // SPEC §6.2 Java: javac -d bin Main.java
            cmd << "mkdir -p bin && javac -d bin Main.java 2>&1";
            break;
        case Language::Python:
            // 解释型：syntactic check only
            cmd << "python3 -m py_compile main.py 2>&1";
            break;
        case Language::Go:
            // SPEC §6.2 Go: go build -o bin main.go
            cmd << "go build -o bin main.go 2>&1";
            break;
        default:
            return "echo 'unsupported language' && false";
    }
    (void)mem_mb;
    return cmd.str();
}

CompileResult compile(Language lang,
                      const std::string& src_dir,
                      const std::string& working_dir,
                      int mem_mb) {
    fs::create_directories(working_dir);
    std::string cmd = make_build_script(lang, src_dir, working_dir, mem_mb);
    return run_shell(cmd, working_dir);
}

}  // namespace judge
