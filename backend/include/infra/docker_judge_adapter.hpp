#pragma once

// =============================================================================
//  oj::infra::DockerJudgeAdapter —— IDockerJudgeClient 的 DockerClient 适配器
//  让 domain::JudgeDispatcher 不直接依赖 DockerClient（保持分层）：
//    JudgeDispatcher --持有--> IDockerJudgeClient (domain 接口)
//                          ↑ 实现
//                       DockerJudgeAdapter (infra 适配器) --持有--> DockerClient
// =============================================================================

#include <memory>

#include "domain/judge_dispatcher.hpp"
#include "infra/docker_client.hpp"

namespace oj::infra {

class DockerJudgeAdapter : public oj::domain::IDockerJudgeClient {
public:
    explicit DockerJudgeAdapter(std::shared_ptr<DockerClient> dc)
        : dc_(std::move(dc)) {}
    ~DockerJudgeAdapter() override = default;

    DockerJudgeAdapter(const DockerJudgeAdapter&)            = delete;
    DockerJudgeAdapter& operator=(const DockerJudgeAdapter&) = delete;

    oj::infra::JudgeResult run(const oj::infra::JudgeTask& task) override {
        return dc_->run(task);
    }

private:
    std::shared_ptr<DockerClient> dc_;
};

}  // namespace oj::infra