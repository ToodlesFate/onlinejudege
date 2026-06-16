#include "domain/auth_service.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace oj::domain {

namespace {

constexpr std::size_t kUsernameMin = 3;
constexpr std::size_t kUsernameMax = 20;
constexpr std::size_t kPasswordMin = 8;
constexpr std::size_t kEmailMax    = 100;

// 业务校验：返回 "" 表示 OK；非空为错误信息
std::string validate_username(std::string_view u) {
    if (u.size() < kUsernameMin || u.size() > kUsernameMax) {
        return "username length must be 3-20 characters";
    }
    for (char c : u) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_';
        if (!ok) {
            return "username may only contain [A-Za-z0-9_]";
        }
    }
    return {};
}

// SPEC §2.1 邮箱只要求「唯一」+ 简单格式校验；
// 这里用宽松的 RFC 5322 风格（<local>@<domain>.<tld>）。
std::string validate_email(std::string_view e) {
    if (e.empty() || e.size() > kEmailMax) {
        return "email length out of range";
    }
    if (e.find('@') == std::string_view::npos) {
        return "email must contain '@'";
    }
    if (e.find('@') != e.rfind('@')) {
        return "email may contain only one '@'";
    }
    if (e.front() == '@' || e.back() == '@' || e.front() == '.' || e.back() == '.') {
        return "email has invalid leading/trailing characters";
    }
    // 至少包含一个 '.'
    if (e.find('.', e.find('@')) == std::string_view::npos) {
        return "email must contain '.' in domain part";
    }
    return {};
}

std::string validate_password(std::string_view p) {
    if (p.size() < kPasswordMin) {
        return "password must be at least 8 characters";
    }
    return {};
}

}  // namespace

RegisterResult AuthService::register_user(std::string_view username,
                                          std::string_view email,
                                          std::string_view password) {
    // 1) 字段校验
    if (auto msg = validate_username(username); !msg.empty()) {
        throw RegisterError(RegisterErrorKind::BadRequest, std::move(msg));
    }
    if (auto msg = validate_email(email); !msg.empty()) {
        throw RegisterError(RegisterErrorKind::BadRequest, std::move(msg));
    }
    if (auto msg = validate_password(password); !msg.empty()) {
        throw RegisterError(RegisterErrorKind::BadRequest, std::move(msg));
    }

    // 2) 重复性预检（明确返回 1005，避免让前端靠 errno 字符串判断）
    if (users_->find_by_username(username).has_value()) {
        throw RegisterError(RegisterErrorKind::Conflict, "username already taken");
    }
    if (users_->find_by_email(email).has_value()) {
        throw RegisterError(RegisterErrorKind::Conflict, "email already taken");
    }

    // 3) 密码哈希
    std::string hash;
    try {
        hash = hasher_->hash(password);
    } catch (const std::exception& e) {
        spdlog::error("AuthService: PasswordHasher.hash failed: {}", e.what());
        throw RegisterError(RegisterErrorKind::Internal, "password hashing failed");
    }

    // 4) DB 插入（事务内 count + insert，首行 is_admin=1）
    oj::domain::User u;
    try {
        u = users_->register_user(username, email, hash);
    } catch (const std::exception& e) {
        // 即使预检通过，理论上并发注册同一 username/email 仍可能 UNIQUE 冲突；
        // 这里退回到"按错误信息嗅探" —— MySQL 1062 = ER_DUP_ENTRY
        const std::string what = e.what();
        if (what.find("username") != std::string::npos) {
            throw RegisterError(RegisterErrorKind::Conflict, "username already taken");
        }
        if (what.find("email") != std::string::npos) {
            throw RegisterError(RegisterErrorKind::Conflict, "email already taken");
        }
        // 兜底：如果是 dup 入口错误但未带具体字段名，视为 conflict
        if (what.find("Duplicate") != std::string::npos ||
            what.find("duplicate") != std::string::npos ||
            what.find("1062")      != std::string::npos) {
            throw RegisterError(RegisterErrorKind::Conflict, "username or email already taken");
        }
        spdlog::error("AuthService: register_user DB error: {}", what);
        throw RegisterError(RegisterErrorKind::Internal, "internal error");
    }

    // 5) 颁发 token
    RegisterResult r;
    r.user_id       = u.id;
    r.is_admin      = u.is_admin;
    r.access_token  = jwt_->issue_access(u.id, u.is_admin);
    r.refresh_token = jwt_->issue_refresh(u.id);
    return r;
}

LoginResult AuthService::login_user(std::string_view username,
                                    std::string_view password) {
    // 1) 字段校验 —— 仅做"非空"检查；不在登录路径做长度/字符集校验
    //    （即使用户名长度非法也交给 401 处理，避免向潜在攻击者泄露
    //    "哪些字段会被服务端解析"）。
    if (username.empty() || password.empty()) {
        throw LoginError(LoginErrorKind::BadRequest,
                         "username and password are required");
    }

    // 2) 查 user（按 username 唯一索引）
    oj::domain::User u;
    try {
        auto found = users_->find_by_username(username);
        if (!found.has_value()) {
            // 用户不存在 —— 与"密码错"统一对外响应，避免用户名枚举
            spdlog::info("AuthService: login failed (unknown user) username='{}'",
                         std::string{username});
            throw LoginError(LoginErrorKind::Unauthorized,
                             "invalid username or password");
        }
        u = *found;
    } catch (const LoginError&) {
        throw;
    } catch (const std::exception& e) {
        spdlog::error("AuthService: login DB lookup error: {}", e.what());
        throw LoginError(LoginErrorKind::Internal, "internal error");
    }

    // 3) 密码校验 —— PasswordHasher::verify 自身是 noexcept + 永不抛
    //    任何"hash 格式异常/被篡改"也只返回 false，统一走 401 路径
    bool ok = false;
    try {
        ok = hasher_->verify(password, u.password_hash);
    } catch (const std::exception& e) {
        // 理论上 verify 不应抛；这里兜底防意外
        spdlog::error("AuthService: hasher.verify threw: {}", e.what());
        throw LoginError(LoginErrorKind::Internal, "internal error");
    }
    if (!ok) {
        spdlog::info("AuthService: login failed (bad password) user_id={} username='{}'",
                     u.id, std::string{username});
        throw LoginError(LoginErrorKind::Unauthorized,
                         "invalid username or password");
    }

    // 4) 颁发 token —— 字段语义与 register() 一致：access 带 adm claim，
    //    refresh 不带（refresh 仅用于换 access，不参与权限判定）
    LoginResult r;
    r.user_id       = u.id;
    r.is_admin      = u.is_admin;
    r.access_token  = jwt_->issue_access(u.id, u.is_admin);
    r.refresh_token = jwt_->issue_refresh(u.id);
    return r;
}

RefreshResult AuthService::refresh_access(std::string_view refresh_token) {
    // 1) 空串直接拒绝 —— Cookie 被清空 / 客户端误传空值时走 BadRequest
    if (refresh_token.empty()) {
        throw RefreshError(RefreshErrorKind::BadRequest, "refresh_token is empty");
    }

    // 2) 校验 JWT：签名 / issuer / exp / type=refresh  —— JwtService::verify
    //    失败统一抛 InvalidToken，这里统一翻译为 Unauthorized。
    //    不区分"过期 vs 篡改 vs type 错"，对外都是 1002 即可（细节不暴露）。
    oj::infra::TokenClaims claims;
    try {
        claims = jwt_->verify(refresh_token, "refresh");
    } catch (const oj::infra::InvalidToken& e) {
        spdlog::info("AuthService: refresh rejected (invalid token): {}", e.what());
        throw RefreshError(RefreshErrorKind::Unauthorized, "invalid refresh token");
    } catch (const std::exception& e) {
        spdlog::error("AuthService: refresh jwt.verify unexpected: {}", e.what());
        throw RefreshError(RefreshErrorKind::Internal, "internal error");
    }

    // 3) 重新查 user —— SPEC §2.1 要求新 access 反映**当前** is_admin
    //    （admin 角色可能已被调整；同时确认用户未被删除）
    oj::domain::User u;
    try {
        auto found = users_->find_by_id(claims.user_id);
        if (!found.has_value()) {
            spdlog::info("AuthService: refresh rejected (user not found) uid={}",
                         claims.user_id);
            throw RefreshError(RefreshErrorKind::Unauthorized, "invalid refresh token");
        }
        u = *found;
    } catch (const RefreshError&) {
        throw;
    } catch (const std::exception& e) {
        spdlog::error("AuthService: refresh DB lookup error: {}", e.what());
        throw RefreshError(RefreshErrorKind::Internal, "internal error");
    }

    // 4) 颁发新 access + 轮换 refresh
    RefreshResult r;
    r.user_id       = u.id;
    r.is_admin      = u.is_admin;
    r.access_token  = jwt_->issue_access(u.id, u.is_admin);
    r.refresh_token = jwt_->issue_refresh(u.id);
    return r;
}

}  // namespace oj::domain
