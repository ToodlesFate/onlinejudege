// =============================================================================
//  test_auth_service.cpp — AuthService 单元测试
//  覆盖：字段校验、查重、首行 is_admin、JWT 颁发
//
//  用 InMemoryUserRepository 替代真实 DB，避免测试依赖外部 MySQL。
// =============================================================================

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "domain/auth_service.hpp"
#include "domain/user_repository.hpp"
#include "infra/jwt_service.hpp"
#include "infra/password_hasher.hpp"

namespace {

using oj::common::JwtConfig;
using oj::domain::AuthService;
using oj::domain::IUserRepository;
using oj::domain::RegisterError;
using oj::domain::RegisterErrorKind;
using oj::domain::User;
using oj::infra::JwtService;
using oj::infra::PasswordHasher;

// 内存版 UserRepo —— 串行化模拟 DB 的 count + insert
class InMemoryUserRepo : public IUserRepository {
public:
    User register_user(std::string_view username,
                       std::string_view email,
                       std::string_view password_hash) override {
        std::lock_guard<std::mutex> lk(mu_);
        // 模拟事务 + FOR UPDATE
        const bool make_admin = users_.empty();
        User u;
        u.id            = ++next_id_;
        u.username      = std::string{username};
        u.email         = std::string{email};
        u.password_hash = std::string{password_hash};
        u.is_admin      = make_admin;
        users_.push_back(u);
        return u;
    }

    std::optional<User> find_by_username(std::string_view username) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& u : users_) {
            if (u.username == username) return u;
        }
        return std::nullopt;
    }

    std::optional<User> find_by_email(std::string_view email) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& u : users_) {
            if (u.email == email) return u;
        }
        return std::nullopt;
    }

    std::optional<User> find_by_id(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& u : users_) {
            if (u.id == id) return u;
        }
        return std::nullopt;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return users_.size();
    }

private:
    mutable std::mutex  mu_;
    std::vector<User>   users_;
    std::int64_t        next_id_{0};
};

JwtConfig make_jwt_cfg() {
    JwtConfig c;
    c.secret          = "test-secret-32-bytes-min-padding-xxx";  // ≥ 32
    c.access_ttl_sec  = 3600;
    c.refresh_ttl_sec = 86400;
    c.issuer          = "onlinejudge-test";
    return c;
}

std::shared_ptr<AuthService> make_service(std::shared_ptr<IUserRepository> repo) {
    auto h = std::make_shared<PasswordHasher>();
    auto j = std::make_shared<JwtService>(make_jwt_cfg());
    return std::make_shared<AuthService>(repo, h, j);
}

// ---------------------------------------------------------------------------
//  字段校验
// ---------------------------------------------------------------------------
TEST(AuthServiceTest, RejectsEmptyUsername) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    try {
        svc->register_user("", "a@b.com", "password123");
        FAIL() << "expected RegisterError";
    } catch (const RegisterError& e) {
        EXPECT_EQ(e.kind(), RegisterErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("username"), std::string::npos);
    }
}

TEST(AuthServiceTest, RejectsShortUsername) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    EXPECT_THROW(svc->register_user("ab", "a@b.com", "password123"), RegisterError);
}

TEST(AuthServiceTest, RejectsLongUsername) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    EXPECT_THROW(svc->register_user(std::string(21, 'a'), "a@b.com", "password123"),
                 RegisterError);
}

TEST(AuthServiceTest, RejectsUsernameWithInvalidChars) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    for (const std::string bad : {"ab!", "name with space", "na-me", "na.me", "用户"}) {
        try {
            svc->register_user(bad, "a@b.com", "password123");
            FAIL() << "expected RegisterError for username=" << bad;
        } catch (const RegisterError& e) {
            EXPECT_EQ(e.kind(), RegisterErrorKind::BadRequest);
        }
    }
}

TEST(AuthServiceTest, AcceptsValidUsernameShapes) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    const std::vector<std::string> good = {
        "abc", "user_name", "User_123", std::string(20, 'x')
    };
    for (std::size_t n = 0; n < good.size(); ++n) {
        svc->register_user(good[n], "e" + std::to_string(n) + "@b.com", "password123");
    }
    EXPECT_EQ(good.size(), 4u);
}

TEST(AuthServiceTest, RejectsEmailWithoutAt) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    try {
        svc->register_user("alice", "not-an-email", "password123");
        FAIL() << "expected RegisterError";
    } catch (const RegisterError& e) {
        EXPECT_EQ(e.kind(), RegisterErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("email"), std::string::npos);
    }
}

TEST(AuthServiceTest, RejectsEmailWithoutDot) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    EXPECT_THROW(svc->register_user("alice", "a@bc", "password123"), RegisterError);
}

TEST(AuthServiceTest, AcceptsValidEmailShapes) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    svc->register_user("alice",   "alice@example.com",     "password123");
    svc->register_user("bob",     "b.o.b@example.co.uk",   "password123");
    svc->register_user("carol",   "carol+tag@sub.x.io",    "password123");
    // 上面不应抛
}

TEST(AuthServiceTest, RejectsShortPassword) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    try {
        svc->register_user("alice", "a@b.com", "short");
        FAIL() << "expected RegisterError";
    } catch (const RegisterError& e) {
        EXPECT_EQ(e.kind(), RegisterErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("password"), std::string::npos);
    }
}

TEST(AuthServiceTest, AcceptsExactlyEightCharPassword) {
    auto svc = make_service(std::make_shared<InMemoryUserRepo>());
    EXPECT_NO_THROW(svc->register_user("alice", "a@b.com", "12345678"));
}

// ---------------------------------------------------------------------------
//  查重
// ---------------------------------------------------------------------------
TEST(AuthServiceTest, RejectsDuplicateUsername) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    svc->register_user("alice", "alice@x.com", "password123");
    try {
        svc->register_user("alice", "different@x.com", "password123");
        FAIL() << "expected RegisterError";
    } catch (const RegisterError& e) {
        EXPECT_EQ(e.kind(), RegisterErrorKind::Conflict);
        EXPECT_NE(std::string{e.what()}.find("username"), std::string::npos);
    }
}

TEST(AuthServiceTest, RejectsDuplicateEmail) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    svc->register_user("alice", "shared@x.com", "password123");
    try {
        svc->register_user("bob", "shared@x.com", "password123");
        FAIL() << "expected RegisterError";
    } catch (const RegisterError& e) {
        EXPECT_EQ(e.kind(), RegisterErrorKind::Conflict);
        EXPECT_NE(std::string{e.what()}.find("email"), std::string::npos);
    }
}

// ---------------------------------------------------------------------------
//  核心：首注册为 admin
// ---------------------------------------------------------------------------
TEST(AuthServiceTest, FirstUserIsAdmin) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    auto r    = svc->register_user("alice", "alice@x.com", "password123");
    EXPECT_TRUE(r.is_admin);
    EXPECT_EQ(r.user_id, 1);
}

TEST(AuthServiceTest, SecondUserIsNotAdmin) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    svc->register_user("alice", "alice@x.com", "password123");
    auto r2 = svc->register_user("bob", "bob@x.com", "password456");
    EXPECT_FALSE(r2.is_admin);
    EXPECT_EQ(r2.user_id, 2);
}

TEST(AuthServiceTest, FirstUserAcrossManyRegistrations) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    bool seen_admin = false;
    for (int i = 0; i < 10; ++i) {
        const std::string uname = "user_" + std::to_string(i);  // ≥3 chars
        const auto r = svc->register_user(
            uname, uname + "@x.com", "password123");
        if (r.is_admin) {
            EXPECT_FALSE(seen_admin) << "more than one admin registered";
            EXPECT_EQ(r.user_id, 1) << "admin must be the first user";
            seen_admin = true;
        }
    }
    EXPECT_TRUE(seen_admin) << "first user should be admin";
    EXPECT_EQ(repo->size(), 10u);
}

// ---------------------------------------------------------------------------
//  返回的 token
// ---------------------------------------------------------------------------
TEST(AuthServiceTest, AccessTokenIsValidJwtWithExpectedClaims) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    auto r    = svc->register_user("alice", "alice@x.com", "password123");

    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    auto claims = jwt->verify(r.access_token, "access");
    EXPECT_EQ(claims.user_id,  r.user_id);
    EXPECT_EQ(claims.is_admin, r.is_admin);
    EXPECT_EQ(claims.type,     "access");
}

TEST(AuthServiceTest, RefreshTokenIsValidAndDifferentFromAccess) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    auto r    = svc->register_user("alice", "alice@x.com", "password123");

    EXPECT_NE(r.access_token, r.refresh_token);

    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    auto access_claims  = jwt->verify(r.access_token,  "access");
    auto refresh_claims = jwt->verify(r.refresh_token, "refresh");
    EXPECT_EQ(access_claims.user_id,  refresh_claims.user_id);
    EXPECT_FALSE(refresh_claims.is_admin);  // refresh 不带 admin 字段
    EXPECT_EQ(refresh_claims.type,     "refresh");
}

TEST(AuthServiceTest, TokensFailVerificationWithWrongSecret) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    auto r    = svc->register_user("alice", "alice@x.com", "password123");

    JwtConfig bad_cfg = make_jwt_cfg();
    bad_cfg.secret    = "a-different-32-byte-secret-pad-pad";  // 长度也够
    auto bad_jwt = std::make_shared<JwtService>(bad_cfg);
    EXPECT_THROW(bad_jwt->verify(r.access_token, "access"), oj::infra::InvalidToken);
    EXPECT_THROW(bad_jwt->verify(r.refresh_token, "refresh"), oj::infra::InvalidToken);
}

TEST(AuthServiceTest, AccessTokenRejectedAsRefresh) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    auto r    = svc->register_user("alice", "alice@x.com", "password123");

    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    EXPECT_THROW(jwt->verify(r.access_token,  "refresh"), oj::infra::InvalidToken);
    EXPECT_THROW(jwt->verify(r.refresh_token, "access"),  oj::infra::InvalidToken);
}

// ---------------------------------------------------------------------------
//  密码以 Argon2id 写入 repo（验证 hash 而非明文）
// ---------------------------------------------------------------------------
TEST(AuthServiceTest, PasswordIsHashedNotStoredAsPlaintext) {
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto svc  = make_service(repo);
    svc->register_user("alice", "alice@x.com", "MySecretPassword123");

    auto u = repo->find_by_username("alice");
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->password_hash.find("MySecretPassword123"), std::string::npos)
        << "plaintext must NOT appear in stored hash";
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(u->password_hash));
    EXPECT_TRUE(PasswordHasher{}.verify("MySecretPassword123", u->password_hash));
    EXPECT_FALSE(PasswordHasher{}.verify("MySecretPassword124", u->password_hash));
}

}  // namespace
