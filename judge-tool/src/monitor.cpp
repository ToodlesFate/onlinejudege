#include "monitor.hpp"
#include <chrono>
#include <fstream>
#include <signal.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace judge {

namespace {

// 读 /proc/<pid>/status 中 VmRSS / VmHWM 字段，返回 kB。失败 -1。
long read_proc_kb(int pid, const char* field) {
    std::ifstream f(std::string("/proc/") + std::to_string(pid) + "/status");
    if (!f) return -1;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(field, 0) == 0) {
            // 例："VmRSS:      1024 kB"
            auto pos = line.find_first_of("0123456789");
            if (pos == std::string::npos) return -1;
            try { return std::stol(line.substr(pos)); } catch (...) { return -1; }
        }
    }
    return -1;
}

}  // namespace

MemoryMonitor::MemoryMonitor(int pid, int poll_ms)
    : pid_(pid), poll_ms_(poll_ms) {}

MemoryMonitor::~MemoryMonitor() {
    stop();
}

void MemoryMonitor::start() {
    if (th_.joinable()) return;
    stop_.store(false, std::memory_order_release);
    th_ = std::thread([this] { run(); });
}

void MemoryMonitor::stop() {
    if (!th_.joinable()) return;
    stop_.store(true, std::memory_order_release);
    th_.join();
}

void MemoryMonitor::run() {
    using namespace std::chrono;
    auto interval = microseconds(poll_ms_ * 1000);

    while (!stop_.load(std::memory_order_acquire)) {
        long rss  = read_proc_kb(pid_, "VmRSS:");
        long hwm  = read_proc_kb(pid_, "VmHWM:");

        // 进程已退出 → /proc/<pid>/status 不存在
        if (rss < 0 && hwm < 0) return;

        long cur = std::max(rss, hwm);
        if (cur > 0) {
            long prev = peak_kb_.load(std::memory_order_relaxed);
            while (cur > prev &&
                   !peak_kb_.compare_exchange_weak(prev, cur,
                                                  std::memory_order_relaxed)) {
                /* retry */
            }
        }

        // 进程还在 → 等下次；进程消失 → 下次循环开头会发现 rss/hwm < 0
        if (kill(pid_, 0) == -1) return;

        std::this_thread::sleep_for(interval);
    }
}

}  // namespace judge
