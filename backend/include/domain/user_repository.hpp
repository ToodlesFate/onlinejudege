#pragma once

// =============================================================================
//  oj::domain::IUserRepository — User 仓储接口（Domain 层抽象）
//  SPEC §3.2.2: UserRepo (MySQL) 属于 Infra 层；但 Domain.AuthService 只依赖
//  本接口，方便单测时用 InMemoryUserRepository 替换真实 DB。
//
//  关键合约：
//    1. register_user() 在一次事务内完成「count + insert」；
//       若调用前 users 表为空，新行 is_admin=1；否则 is_admin=0。
//       内部用 "SELECT ... FOR UPDATE" 锁表避免并发双 admin。
//    2. find_by_*() 仅用于登录 / 查重；返回 std::optional<User>。
//    3. 所有方法抛 std::runtime_error 表示 DB 错误。
// =============================================================================

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oj::domain {

struct User {
    std::int64_t id{};
    std::string  username;
    std::string  email;
    std::string  password_hash;
    bool         is_admin{false};
};

class IUserRepository {
public:
    virtual ~IUserRepository() = default;

    // 注册：在事务内 count + insert；首行 → is_admin=true
    // 抛 std::runtime_error 表示 DB 错误；UNIQUE 冲突由调用方按 errno 区分
    virtual User register_user(std::string_view username,
                               std::string_view email,
                               std::string_view password_hash) = 0;

    virtual std::optional<User> find_by_username(std::string_view username) = 0;

    virtual std::optional<User> find_by_email(std::string_view email) = 0;

    virtual std::optional<User> find_by_id(std::int64_t id) = 0;
};

}  // namespace oj::domain
