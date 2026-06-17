#pragma once

// =============================================================================
//  oj::infra::MysqlProblemRepo —— libmysqlclient 实现的 IProblemRepository
//  SPEC §3.2.2 "ProblemRepo (MySQL)" / §4.2 problems 表
//  SQL 全部走 mysql_real_query + 手动取列 (与 user_repo 风格一致)
//
//  注意：tag 关联不通过本类管理 —— 走 ITagRepository::set_problem_tags。
//  本类 create()/update() 只写 problems 行；调用方负责"先 create problem
//  → 再 set_problem_tags" 的两阶段调用。
// =============================================================================

#include <memory>

#include "domain/problem_repository.hpp"
#include "infra/mysql_client.hpp"

namespace oj::infra {

class MysqlProblemRepo : public oj::domain::IProblemRepository {
public:
    explicit MysqlProblemRepo(std::shared_ptr<MysqlClient> mysql)
        : mysql_(std::move(mysql)) {}
    ~MysqlProblemRepo() override = default;

    MysqlProblemRepo(const MysqlProblemRepo&)            = delete;
    MysqlProblemRepo& operator=(const MysqlProblemRepo&) = delete;

    std::optional<oj::domain::Problem> find_by_id(std::int64_t id) override;
    oj::domain::ProblemListResult list(const oj::domain::ProblemListQuery& q) override;
    oj::domain::Problem create(const oj::domain::Problem& p) override;
    void update(const oj::domain::Problem& p) override;
    void soft_delete(std::int64_t id) override;
    void set_published(std::int64_t id, bool published) override;
    std::pair<int, int> submission_stats(std::int64_t problem_id) override;

private:
    std::shared_ptr<MysqlClient> mysql_;
};

}  // namespace oj::infra
