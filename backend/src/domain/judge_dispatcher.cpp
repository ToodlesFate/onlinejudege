// =============================================================================
//  JudgeDispatcher 实现 —— SPEC §3.2.2 / §6.1
//
//  流程：
//    worker_loop():
//      while (!stopping_):
//        try:
//          ClaimedTask t;
//          if (repo->claim_one(t)):
//            ++dispatched_count_;
//            process_one(t);
//        catch (const std::exception& e):
//          log; sleep poll_interval; continue
//
//    process_one():
//      JudgeTaskPayload p = repo->load_task(id);
//      JudgeTask task { ... };
//      JudgeResult r = docker->run(task);
//      // 把 cases 落库
//      for (c in r.cases): repo->insert_case(id, c);
//      // 把 submission 终态落库
//      repo->finish(id, result_from_judge(r.overall), r.total_score, ...);
//      ++finished_count_;
//
//  重要：
//    - DockerClient::run() 任何阶段失败都返回 JudgeStatus::SE，judge_message
//      已带原因；我们不需要 catch run() 的异常
//    - 状态机：claim_one 已经把 status 改成 'running'，所以这里不再调
//      update_status(running)；直接跑 + finish
//    - 在 status='compiling' 这层本设计省略：claim 直接跳到 running，
//      状态机展示的 compiling/running 在视觉上合并
// =============================================================================

#include "domain/judge_dispatcher.hpp"

#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "domain/submission_types.hpp"

namespace oj::domain {

namespace {

// 把 infra::JudgeStatus 映射到 domain::SubmissionResult
SubmissionResult judge_status_to_result(oj::infra::JudgeStatus s) noexcept {
    switch (s) {
        case oj::infra::JudgeStatus::AC:  return SubmissionResult::AC;
        case oj::infra::JudgeStatus::WA:  return SubmissionResult::WA;
        case oj::infra::JudgeStatus::TLE: return SubmissionResult::TLE;
        case oj::infra::JudgeStatus::MLE: return SubmissionResult::MLE;
        case oj::infra::JudgeStatus::OLE: return SubmissionResult::OLE;
        case oj::infra::JudgeStatus::RE:  return SubmissionResult::RE;
        case oj::infra::JudgeStatus::CE:  return SubmissionResult::CE;
        case oj::infra::JudgeStatus::SE:  return SubmissionResult::SE;
    }
    return SubmissionResult::SE;
}

// 把 domain::Language 映射到 infra::SubmissionLanguage
oj::infra::SubmissionLanguage domain_lang_to_infra(Language l) noexcept {
    switch (l) {
        case Language::C:      return oj::infra::SubmissionLanguage::C;
        case Language::Cpp:    return oj::infra::SubmissionLanguage::Cpp;
        case Language::Java:   return oj::infra::SubmissionLanguage::Java;
        case Language::Python: return oj::infra::SubmissionLanguage::Python;
        case Language::Go:     return oj::infra::SubmissionLanguage::Go;
    }
    return oj::infra::SubmissionLanguage::Cpp;
}

}  // namespace

JudgeDispatcher::JudgeDispatcher(common::JudgeConfig cfg,
                                 std::shared_ptr<ISubmissionRepository> repo,
                                 std::shared_ptr<IDockerJudgeClient> docker)
    : cfg_(std::move(cfg)),
      repo_(std::move(repo)),
      docker_(std::move(docker)),
      poll_interval_(std::chrono::milliseconds(cfg_.poll_interval_ms)) {
    if (!repo_)   throw std::runtime_error("JudgeDispatcher: repo is null");
    if (!docker_) throw std::runtime_error("JudgeDispatcher: docker is null");
    if (cfg_.worker_count < 1) {
        spdlog::warn("JudgeDispatcher: worker_count < 1; clamping to 1");
        cfg_.worker_count = 1;
    }
}

JudgeDispatcher::~JudgeDispatcher() {
    stop();
}

void JudgeDispatcher::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return;  // 已 start
    }
    stopping_.store(false, std::memory_order_release);

    workers_.reserve(static_cast<std::size_t>(cfg_.worker_count));
    for (int i = 0; i < cfg_.worker_count; ++i) {
        workers_.emplace_back([this, i] { worker_loop(i); });
    }
    spdlog::info("JudgeDispatcher::start workers={} poll_ms={}",
                 cfg_.worker_count, poll_interval_.count());
}

void JudgeDispatcher::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return;  // 已 stop 或从未 start
    }
    stopping_.store(true, std::memory_order_release);

    for (auto& t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    // 兜底：所有 status='running' 的 submission 改成 SE
    mark_running_to_se("dispatcher stopped");

    spdlog::info("JudgeDispatcher::stop complete");
}

void JudgeDispatcher::worker_loop(int worker_id) {
    spdlog::debug("JudgeDispatcher::worker[{}] start", worker_id);

    while (!stopping_.load(std::memory_order_acquire)) {
        active_workers_.fetch_add(1, std::memory_order_acq_rel);

        ClaimedTask claim;
        try {
            if (repo_->claim_one(claim)) {
                ++dispatched_count_;
                try {
                    process_one(claim);
                } catch (const std::exception& e) {
                    // process_one 内部已经尝试落库；这里只兜底：万一 finish 失败
                    spdlog::error("JudgeDispatcher::process_one crashed id={} err={}",
                                  claim.submission_id, e.what());
                    try {
                        repo_->finish(claim.submission_id,
                                      SubmissionResult::SE,
                                      /*score=*/0,
                                      /*time_ms=*/0,
                                      /*mem_kb=*/0,
                                      /*compile_output=*/"",
                                      std::string{"dispatcher internal error: "} + e.what());
                    } catch (...) {
                        spdlog::critical("JudgeDispatcher::finish also failed id={}",
                                         claim.submission_id);
                    }
                    ++finished_count_;
                }
            } else {
                // 没抢到任务 → sleep poll_interval 再试
                std::this_thread::sleep_for(poll_interval_);
            }
        } catch (const std::exception& e) {
            // claim_one 抛了 (DB 异常) → sleep 再试，避免空转打爆日志
            spdlog::warn("JudgeDispatcher::claim_one err={} (sleep {} ms)",
                         e.what(), poll_interval_.count());
            std::this_thread::sleep_for(poll_interval_);
        }

        active_workers_.fetch_sub(1, std::memory_order_acq_rel);
    }

    spdlog::debug("JudgeDispatcher::worker[{}] exit", worker_id);
}

void JudgeDispatcher::process_one(const ClaimedTask& claim) {
    // 1) 加载完整任务（problem limits + testcases）
    auto payload = repo_->load_task(claim.submission_id);

    // 2) 组装 JudgeTask
    oj::infra::JudgeTask task;
    task.submission_id = claim.submission_id;
    task.language      = domain_lang_to_infra(claim.language);
    task.code          = claim.code;
    task.testcases     = std::move(payload.testcases);
    task.limits.time_ms = payload.time_limit_ms;
    task.limits.mem_mb  = payload.memory_limit_mb;
    task.limits.out_mb  = payload.output_limit_mb;
    task.image          = image_for_language(claim.language);

    // 3) 判题
    oj::infra::JudgeResult jr;
    try {
        jr = docker_->run(task);
    } catch (const std::exception& e) {
        // IDockerJudgeClient::run() 应当不抛；这里是双保险
        jr.overall = oj::infra::JudgeStatus::SE;
        jr.judge_message = std::string{"docker client threw: "} + e.what();
    }

    // 4) 写 submission_cases
    for (const auto& c : jr.cases) {
        oj::domain::SubmissionCase row;
        row.submission_id = claim.submission_id;
        row.case_index    = c.index;
        row.status        = judge_status_to_result(c.status);
        row.time_used_ms  = c.time_ms;
        row.memory_used_kb = static_cast<int>(c.mem_kb);
        row.score         = c.score;
        row.is_sample     = c.is_sample;
        if (c.is_sample) {
            row.user_output = c.user_output;
        } else {
            row.user_output.clear();
        }
        try {
            repo_->insert_case(claim.submission_id, row);
        } catch (const std::exception& e) {
            spdlog::warn("JudgeDispatcher::insert_case id={} idx={} err={}",
                         claim.submission_id, c.index, e.what());
        }
    }

    // 5) 写 submission 终态
    //    judge_message 截断到 500 字符（repo 也兜底一次）
    std::string msg = jr.judge_message;
    if (msg.size() > 500) msg.resize(500);

    // compile_output 同样截断（MEDIUMTEXT 64MB，但太大仍不建议）
    // 这里不截断，依赖 MEDIUMTEXT 的容量；> 64MB 才需裁

    SubmissionResult result = judge_status_to_result(jr.overall);

    repo_->finish(claim.submission_id,
                  result,
                  jr.total_score,
                  jr.time_ms,
                  static_cast<int>(jr.mem_kb),
                  jr.compile_output,
                  msg);
    ++finished_count_;

    spdlog::info("JudgeDispatcher::process_one id={} result={} score={}",
                 claim.submission_id, oj::domain::to_string(result), jr.total_score);
}

std::string JudgeDispatcher::image_for_language(Language l) const {
    switch (l) {
        case Language::C:      return cfg_.images.c;
        case Language::Cpp:    return cfg_.images.cpp;
        case Language::Java:   return cfg_.images.java;
        case Language::Python: return cfg_.images.python;
        case Language::Go:     return cfg_.images.go;
    }
    return {};
}

void JudgeDispatcher::mark_running_to_se(const std::string& reason) {
    try {
        repo_->mark_all_running_as_se_on_shutdown(reason);
    } catch (const std::exception& e) {
        spdlog::error("JudgeDispatcher::mark_running_to_se failed: {}", e.what());
    }
}

}  // namespace oj::domain