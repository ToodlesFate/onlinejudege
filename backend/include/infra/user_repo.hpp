#pragma once

// =============================================================================
//  oj::infra::MysqlUserRepo — libmysqlclient 实现的 IUserRepository
//  SPEC §3.2.2 "UserRepo (MySQL)"
//  实现细节见 cpp
// =============================================================================

#include "domain/user_repository.hpp"
#include "infra/mysql_client.hpp"

#include <memory>

namespace oj::infra {

class MysqlUserRepo : public oj::domain::IUserRepository {
public:
    explicit MysqlUserRepo(std::shared_ptr<MysqlClient> mysql)
        : mysql_(std::move(mysql)) {}
    ~MysqlUserRepo() override = default;

    MysqlUserRepo(const MysqlUserRepo&)            = delete;
    MysqlUserRepo& operator=(const MysqlUserRepo&) = delete;

    oj::domain::User register_user(std::string_view username,
                                   std::string_view email,
                                   std::string_view password_hash) override;

    std::optional<oj::domain::User> find_by_username(std::string_view username) override;
    std::optional<oj::domain::User> find_by_email(std::string_view email) override;
    std::optional<oj::domain::User> find_by_id(std::int64_t id) override;

private:
    // 单次 register；register_user 外层会捕获 deadlock 自动重试
    oj::domain::User register_user_once(std::string_view username,
                                        std::string_view email,
                                        std::string_view password_hash);
    std::shared_ptr<MysqlClient> mysql_;
};

}  // namespace oj::infra
