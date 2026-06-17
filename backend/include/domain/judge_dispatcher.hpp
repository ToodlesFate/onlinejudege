#pragma once

// =============================================================================
//  oj::domain::JudgeDispatcher —— 后台判题线程池
//  SPEC §3.2.2 "JudgeDispatcher" / §6.1 完整判题工作流 / §2.3.2 状态机
//
//  职责：
//    1) 启动 N 个 worker 线程（默认 4，由 cfg.judge.worker_count 决定）
//    2) worker 不停地通过 ISubmissionRepository::claim_one() 抢任务
//       (FOR UPDATE SKIP LOCKED 让多 worker 并发抢不互锁)
//    3) 抢到后调 IDockerJudgeClient::run(JudgeTask) 执行判题
//    4) 把结果落库：
//         - submission_cases 逐点行
//         - submissions 终态 (status / result / total_score / ...)
//    5) graceful shutdown：stop() 等所有 worker 退出后，把 status='running'
//       的 submission 标记为 SE (因为容器已被中断/未完成)
//
//  线程模型：
//    - worker 用 std::thread + std::mutex + std::condition_variable 实现
//    - 抢不到任务时 worker 会 wait_for(poll_interval_ms) 再抢，避免空转
//    - 没有任何跨线程共享可变状态；所有 worker 只通过 repo / docker 通信
//
//  设计要点：
//    - JudgeDispatcher 不依赖具体 IDockerJudgeClient 实现，只通过接口取；
//      测试时可注入 MockDockerClient（in-process 实现接口即可）。
//    - JudgeDispatcher 不持有 ISubmissionRepository 的所有权，只持 shared_ptr；
//      测试可同 repo 注入。
//    - 不引入第三方线程库；只用 std 库的 thread / mutex / cv。
// =============================================================================

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "common/config.hpp"
#include "domain/submission_repository.hpp"
#include "infra/docker_client.hpp"

namespace oj::domain {

// ---------------------------------------------------------------------------
//  IDockerJudgeClient —— docker 客户端的接口抽象（仅暴露判题 run）
//  DockerClient::run() 已经是 const member + 无副作用，但为了便于测试时
//  注入 in-process Mock，这里再抽象一层。
// ---------------------------------------------------------------------------
class IDockerJudgeClient {
public:
    virtual ~IDockerJudgeClient() = default;
    virtual oj::infra::JudgeResult run(const oj::infra::JudgeTask& task) = 0;
};

// ---------------------------------------------------------------------------
//  JudgeDispatcher
// ---------------------------------------------------------------------------
class JudgeDispatcher {
public:
    JudgeDispatcher(common::JudgeConfig cfg,
                    std::shared_ptr<ISubmissionRepository> repo,
                    std::shared_ptr<IDockerJudgeClient> docker);
    ~JudgeDispatcher();

    JudgeDispatcher(const JudgeDispatcher&)            = delete;
    JudgeDispatcher& operator=(const JudgeDispatcher&) = delete;

    /** 启动所有 worker 线程（idempotent：start() 之后再次 start() 不会重启线程） */
    void start();

    /**
     * 优雅停机：
     *   1) 设 stopping_=true，唤醒所有 worker
     *   2) join 所有 worker
     *   3) 把 status='running' 的 submission 标记为 SE
     * idempotent。
     */
    void stop();

    /** 是否已 start */
    [[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /** 当前在跑的 worker 数（便于测试断言） */
    [[nodiscard]] int  active_workers() const noexcept {
        return active_workers_.load(std::memory_order_acquire);
    }

    /** 已派发（claim 成功）的总任务数（仅测试用） */
    [[nodiscard]] std::int64_t dispatched_count() const noexcept {
        return dispatched_count_.load(std::memory_order_acquire);
    }

    /** 已完成（落库 finish）的总任务数（仅测试用） */
    [[nodiscard]] std::int64_t finished_count() const noexcept {
        return finished_count_.load(std::memory_order_acquire);
    }

    // 测试钩：注入更短的 poll interval 以加速用例
    void set_poll_interval_for_test(std::chrono::milliseconds ms) noexcept {
        poll_interval_ = ms;
    }

private:
    void worker_loop(int worker_id);
    void process_one(const ClaimedTask& claim);
    void mark_running_to_se(const std::string& reason);

    // 给 run() 用：把 language → image
    std::string image_for_language(Language l) const;

    common::JudgeConfig                       cfg_;
    std::shared_ptr<ISubmissionRepository>    repo_;
    std::shared_ptr<IDockerJudgeClient>       docker_;
    std::chrono::milliseconds                 poll_interval_{500};

    std::vector<std::thread>                  workers_;
    std::atomic<bool>                         running_{false};
    std::atomic<bool>                         stopping_{false};
    std::atomic<int>                          active_workers_{0};
    std::atomic<std::int64_t>                 dispatched_count_{0};
    std::atomic<std::int64_t>                 finished_count_{0};
};

}  // namespace oj::domain