#pragma once

// =============================================================================
//  oj::infra::MysqlTestcaseRepo —— libmysqlclient 实现的 ITestcaseRepository
//  SPEC §3.2.2 + §4.2 testcases 表
// =============================================================================

#include <memory>

#include "domain/testcase_repository.hpp"
#include "infra/mysql_client.hpp"

namespace oj::infra {

class MysqlTestcaseRepo : public oj::domain::ITestcaseRepository {
public:
    explicit MysqlTestcaseRepo(std::shared_ptr<MysqlClient> mysql)
        : mysql_(std::move(mysql)) {}
    ~MysqlTestcaseRepo() override = default;

    MysqlTestcaseRepo(const MysqlTestcaseRepo&)            = delete;
    MysqlTestcaseRepo& operator=(const MysqlTestcaseRepo&) = delete;

    std::vector<oj::domain::Testcase> list_by_problem(std::int64_t problem_id) override;
    std::vector<oj::domain::Testcase> list_samples(std::int64_t problem_id) override;
    void create_many(std::int64_t problem_id,
                     const std::vector<oj::domain::Testcase>& cases) override;
    void replace_by_problem(std::int64_t problem_id,
                            const std::vector<oj::domain::Testcase>& cases) override;
    void delete_by_problem(std::int64_t problem_id) override;

private:
    std::shared_ptr<MysqlClient> mysql_;
};

}  // namespace oj::infra
