#pragma once

// =============================================================================
//  oj::domain::AuthService — 注册 / 登录 / 刷新领域逻辑
//  SPEC §2.1 / §3.2.2 "AuthService"
//  本阶段实现 register() + login()；refresh() 留作后续 Phase 2 子项。
//
//  设计要点：
//    1. 构造时注入 IUserRepository + PasswordHasher + JwtService —— 全部
//       由调用方（main.cpp）组装，符合 SPEC §3.2.1 单向依赖（Http → Domain ← Infra）
//    2. register() 内部完成：
//       a) 字段校验（username 3-20 / [A-Za-z0-9_]；email 简单格式；
//          password ≥ 8 字符）
//       b) 重复性预检（find_by_username / find_by_email）—— 避免依赖 errno 解析
//       c) PasswordHasher.hash()
//       d) UserRepo.register_user() —— 内部事务保证「首行 is_admin=1」
//       e) JwtService.issue_access() / issue_refresh()
//    3. 错误用 RegisterError 表达，handler 层统一映射 ErrorCode
// =============================================================================

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "domain/user_repository.hpp"
#include "infra/jwt_service.hpp"
#include "infra/password_hasher.hpp"

namespace oj::domain {

// 业务校验失败的细分类；handler 按 kind 映射到 ErrorCode
enum class RegisterErrorKind {
    BadRequest,    // → 1001
    Conflict,      // → 1005
    Internal,      // → 1007
};

class RegisterError : public std::runtime_error {
public:
    RegisterError(RegisterErrorKind k, std::string msg)
        : std::runtime_error(std::move(msg)), kind_(k) {}
    [[nodiscard]] RegisterErrorKind kind() const noexcept { return kind_; }
private:
    RegisterErrorKind kind_;
};

struct RegisterResult {
    std::int64_t user_id{};
    std::string  access_token;
    std::string  refresh_token;
    bool         is_admin{false};
};

// 登录流程的细分类：与 RegisterErrorKind 区分开以让 handler 一目了然
enum class LoginErrorKind {
    BadRequest,    // → 1001  (username/password 缺失)
    Unauthorized,  // → 1002  (用户不存在 / 密码错误 —— 对外统一为"invalid credentials"以避免用户名枚举)
    Internal,      // → 1007
};

class LoginError : public std::runtime_error {
public:
    LoginError(LoginErrorKind k, std::string msg)
        : std::runtime_error(std::move(msg)), kind_(k) {}
    [[nodiscard]] LoginErrorKind kind() const noexcept { return kind_; }
private:
    LoginErrorKind kind_;
};

struct LoginResult {
    std::int64_t user_id{};
    std::string  access_token;
    std::string  refresh_token;
    bool         is_admin{false};
};

class AuthService {
public:
    AuthService(std::shared_ptr<IUserRepository> users,
                std::shared_ptr<infra::PasswordHasher> hasher,
                std::shared_ptr<infra::JwtService> jwt)
        : users_(std::move(users)),
          hasher_(std::move(hasher)),
          jwt_(std::move(jwt)) {}

    // 注册流程，失败抛 RegisterError
    [[nodiscard]] RegisterResult register_user(std::string_view username,
                                              std::string_view email,
                                              std::string_view password);

    // 登录流程，失败抛 LoginError
    //   - BadRequest     : username / password 为空
    //   - Unauthorized   : 用户不存在 / 密码错误 —— 出于安全统一返回 1002，
    //                      message 一律 "invalid username or password"，
    //                      避免泄露用户名是否存在
    [[nodiscard]] LoginResult login_user(std::string_view username,
                                        std::string_view password);

    // 暴露给 handler 用来把 Refresh Token 写入 Set-Cookie 时附带 Max-Age
    [[nodiscard]] int refresh_ttl_sec() const noexcept {
        return jwt_->refresh_ttl_sec();
    }

private:
    std::shared_ptr<IUserRepository>    users_;
    std::shared_ptr<infra::PasswordHasher> hasher_;
    std::shared_ptr<infra::JwtService>  jwt_;
};

}  // namespace oj::domain
