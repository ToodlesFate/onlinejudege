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

}  // namespace oj::domain
