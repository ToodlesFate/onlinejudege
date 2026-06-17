#pragma once

// =============================================================================
//  oj::infra::MysqlTagRepo —— libmysqlclient 实现的 ITagRepository
//  SPEC §3.2.2 + §4.2 tags / problem_tags 表
// =============================================================================

#include <memory>

#include "domain/tag_repository.hpp"
#include "infra/mysql_client.hpp"

namespace oj::infra {

class MysqlTagRepo : public oj::domain::ITagRepository {
public:
    explicit MysqlTagRepo(std::shared_ptr<MysqlClient> mysql)
        : mysql_(std::move(mysql)) {}
    ~MysqlTagRepo() override = default;

    MysqlTagRepo(const MysqlTagRepo&)            = delete;
    MysqlTagRepo& operator=(const MysqlTagRepo&) = delete;

    std::vector<oj::domain::Tag> list_all() override;
    std::optional<oj::domain::Tag> find_by_id(int id) override;
    std::optional<oj::domain::Tag> find_by_slug(const std::string& slug) override;
    std::vector<oj::domain::Tag> find_by_ids(const std::vector<int>& ids) override;
    std::vector<oj::domain::Tag> tags_of_problem(std::int64_t problem_id) override;
    std::vector<int> tag_ids_of_problem(std::int64_t problem_id) override;
    void set_problem_tags(std::int64_t problem_id,
                          const std::vector<int>& tag_ids) override;

private:
    std::shared_ptr<MysqlClient> mysql_;
};

}  // namespace oj::infra
