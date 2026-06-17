#pragma once

// =============================================================================
//  oj::infra::MysqlSubmissionRepo —— libmysqlclient 实现的 ISubmissionRepository
//  SPEC §3.2.2 / §4.2 submissions / submission_cases 表
//  风格与 MysqlUserRepo / MysqlProblemRepo 完全一致：
//    1) escape + quote 拼 SQL
//    2) START TRANSACTION / COMMIT / ROLLBACK with RollbackGuard
//    3) deadlock / lock-wait 自动重试（最大 3 次）
// =============================================================================

#include <memory>

#include "domain/submission_repository.hpp"
#include "infra/mysql_client.hpp"

namespace oj::infra {

class MysqlSubmissionRepo : public oj::domain::ISubmissionRepository {
public:
    explicit MysqlSubmissionRepo(std::shared_ptr<MysqlClient> mysql)
        : mysql_(std::move(mysql)) {}
    ~MysqlSubmissionRepo() override = default;

    MysqlSubmissionRepo(const MysqlSubmissionRepo&)            = delete;
    MysqlSubmissionRepo& operator=(const MysqlSubmissionRepo&) = delete;

    std::int64_t create(std::int64_t user_id,
                        std::int64_t problem_id,
                        oj::domain::Language language,
                        std::string_view code) override;
    bool claim_one(oj::domain::ClaimedTask& out) override;
    oj::domain::JudgeTaskPayload load_task(std::int64_t submission_id) override;
    void update_status(std::int64_t submission_id,
                       oj::domain::SubmissionStatus new_status) override;
    void finish(std::int64_t submission_id,
                oj::domain::SubmissionResult result,
                int total_score,
                int time_used_ms,
                int memory_used_kb,
                std::string_view compile_output,
                std::string_view judge_message) override;
    void insert_case(std::int64_t submission_id,
                     const oj::domain::SubmissionCase& c) override;
    void mark_all_running_as_se_on_shutdown(std::string_view reason) override;
    std::optional<oj::domain::Submission> find_by_id(std::int64_t id) override;
    std::optional<oj::domain::SubmissionDetail> get_full(std::int64_t id) override;
    oj::domain::SubmissionListResult list_by_user(
        const oj::domain::SubmissionListQuery& q) override;
    oj::domain::SubmissionListResult list_public_accepted(
        const oj::domain::SubmissionListQuery& q) override;

private:
    std::shared_ptr<MysqlClient> mysql_;
};

}  // namespace oj::infra