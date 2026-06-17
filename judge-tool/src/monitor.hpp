#pragma once
#include <atomic>
#include <thread>

namespace judge {

// 内存监控线程：周期性读 /proc/<pid>/status 的 VmRSS / VmHWM，
// 把峰值 KB 写到 peak_kb。Linux-only。
// pid == 0 或进程消失 → 线程自然退出。
class MemoryMonitor {
public:
    explicit MemoryMonitor(int pid, int poll_ms = 10);
    ~MemoryMonitor();

    void start();
    void stop();

    long peak_kb() const noexcept { return peak_kb_.load(std::memory_order_relaxed); }

private:
    void run();

    int               pid_;
    int               poll_ms_;
    std::atomic<long> peak_kb_{0};
    std::atomic<bool> stop_{false};
    std::thread       th_;
};

}  // namespace judge
