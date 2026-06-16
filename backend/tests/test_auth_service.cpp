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
using oj::domain::LoginError;
using oj::domain::LoginErrorKind;
using oj::domain::RefreshError;
using oj::domain::RefreshErrorKind;
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

    // 仅供测试：按 id 抹掉一个 user（用来验证 "refresh 有效但 user 已删" 分支）
    // 不暴露在 IUserRepository 接口里 —— 业务上 v1 不支持删 user
    void remove_for_test(std::int64_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = users_.begin(); it != users_.end(); ++it) {
            if (it->id == id) { users_.erase(it); return; }
        }
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

// ===========================================================================
//  login_user()  ——  SPEC §5.2.1 POST /api/auth/login
// ===========================================================================

// 准备一个含一个用户（"alice"/"password123"）的 service
struct LoginFixture {
    std::shared_ptr<InMemoryUserRepo> repo = std::make_shared<InMemoryUserRepo>();
    std::shared_ptr<AuthService>      svc;
    LoginFixture() {
        auto h = std::make_shared<PasswordHasher>();
        auto j = std::make_shared<JwtService>(make_jwt_cfg());
        svc = std::make_shared<AuthService>(repo, h, j);
        svc->register_user("alice", "alice@x.com", "password123");
    }
};

// ---------------------------------------------------------------------------
//  输入校验
// ---------------------------------------------------------------------------
TEST(AuthServiceLoginTest, RejectsEmptyUsername) {
    LoginFixture f;
    try {
        f.svc->login_user("", "password123");
        FAIL() << "expected LoginError";
    } catch (const LoginError& e) {
        EXPECT_EQ(e.kind(), LoginErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("username"), std::string::npos);
    }
}

TEST(AuthServiceLoginTest, RejectsEmptyPassword) {
    LoginFixture f;
    try {
        f.svc->login_user("alice", "");
        FAIL() << "expected LoginError";
    } catch (const LoginError& e) {
        EXPECT_EQ(e.kind(), LoginErrorKind::BadRequest);
        EXPECT_NE(std::string{e.what()}.find("password"), std::string::npos);
    }
}

TEST(AuthServiceLoginTest, RejectsBothEmpty) {
    LoginFixture f;
    EXPECT_THROW(f.svc->login_user("", ""), LoginError);
}

// ---------------------------------------------------------------------------
//  用户不存在 / 密码错 —— 统一 Unauthorized + 同一 message
// ---------------------------------------------------------------------------
TEST(AuthServiceLoginTest, UnknownUsernameIsUnauthorizedWithGenericMessage) {
    LoginFixture f;
    try {
        f.svc->login_user("nobody", "password123");
        FAIL() << "expected LoginError";
    } catch (const LoginError& e) {
        EXPECT_EQ(e.kind(), LoginErrorKind::Unauthorized);
        EXPECT_EQ(std::string{e.what()}, "invalid username or password");
    }
}

TEST(AuthServiceLoginTest, WrongPasswordIsUnauthorizedWithGenericMessage) {
    LoginFixture f;
    try {
        f.svc->login_user("alice", "wrong-password");
        FAIL() << "expected LoginError";
    } catch (const LoginError& e) {
        EXPECT_EQ(e.kind(), LoginErrorKind::Unauthorized);
        EXPECT_EQ(std::string{e.what()}, "invalid username or password");
    }
}

TEST(AuthServiceLoginTest, UnknownUserAndWrongPasswordProduceSameMessage) {
    // 防用户名枚举：两种"失败"原因对外必须完全相同
    LoginFixture f;
    std::string msg_unknown, msg_wrong;
    try { f.svc->login_user("nobody", "x"); }
    catch (const LoginError& e) { msg_unknown = e.what(); }
    try { f.svc->login_user("alice", "wrong"); }
    catch (const LoginError& e) { msg_wrong = e.what(); }
    EXPECT_EQ(msg_unknown, msg_wrong);
    EXPECT_FALSE(msg_unknown.empty());
}

// ---------------------------------------------------------------------------
//  Happy path
// ---------------------------------------------------------------------------
TEST(AuthServiceLoginTest, ValidCredentialsReturnUserIdIsAdminAndTokens) {
    LoginFixture f;
    auto r = f.svc->login_user("alice", "password123");
    EXPECT_GT(r.user_id, 0);
    EXPECT_TRUE(r.is_admin);   // alice 是该 repo 的首行 → admin（fixture 内她是唯一用户）
    EXPECT_FALSE(r.access_token.empty());
    EXPECT_FALSE(r.refresh_token.empty());
    EXPECT_NE(r.access_token, r.refresh_token);
}

TEST(AuthServiceLoginTest, LoginIsAdminFlagMatchesStoredUser) {
    // 首个注册用户就是 admin —— 登录也应原样返回 is_admin=true
    auto repo = std::make_shared<InMemoryUserRepo>();
    auto h    = std::make_shared<PasswordHasher>();
    auto j    = std::make_shared<JwtService>(make_jwt_cfg());
    auto svc  = std::make_shared<AuthService>(repo, h, j);
    svc->register_user("admin1", "admin1@x.com", "password123");
    auto r = svc->login_user("admin1", "password123");
    EXPECT_TRUE(r.is_admin);
}

// ---------------------------------------------------------------------------
//  颁发出来的 token 真的是合法 JWT，且能解出正确 claims
// ---------------------------------------------------------------------------
TEST(AuthServiceLoginTest, LoginIssuedAccessTokenVerifiesWithExpectedClaims) {
    LoginFixture f;
    auto r = f.svc->login_user("alice", "password123");
    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    auto claims = jwt->verify(r.access_token, "access");
    EXPECT_EQ(claims.user_id,  r.user_id);
    EXPECT_EQ(claims.is_admin, r.is_admin);
    EXPECT_EQ(claims.type,     "access");
}

TEST(AuthServiceLoginTest, LoginIssuedRefreshTokenVerifiesAsRefreshType) {
    LoginFixture f;
    auto r = f.svc->login_user("alice", "password123");
    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    auto claims = jwt->verify(r.refresh_token, "refresh");
    EXPECT_EQ(claims.user_id, r.user_id);
    EXPECT_FALSE(claims.is_admin);  // refresh 不带 adm
    EXPECT_EQ(claims.type,    "refresh");
}

TEST(AuthServiceLoginTest, LoginAccessTokenRejectedAsRefreshAndViceVersa) {
    LoginFixture f;
    auto r = f.svc->login_user("alice", "password123");
    auto jwt = std::make_shared<JwtService>(make_jwt_cfg());
    EXPECT_THROW(jwt->verify(r.access_token,  "refresh"), oj::infra::InvalidToken);
    EXPECT_THROW(jwt->verify(r.refresh_token, "access"),  oj::infra::InvalidToken);
}

TEST(AuthServiceLoginTest, LoginTokensFailVerificationWithWrongSecret) {
    LoginFixture f;
    auto r = f.svc->login_user("alice", "password123");
    JwtConfig bad_cfg = make_jwt_cfg();
    bad_cfg.secret    = "a-different-32-byte-secret-pad-pad";
    auto bad_jwt = std::make_shared<JwtService>(bad_cfg);
    EXPECT_THROW(bad_jwt->verify(r.access_token,  "access"),  oj::infra::InvalidToken);
    EXPECT_THROW(bad_jwt->verify(r.refresh_token, "refresh"), oj::infra::InvalidToken);
}

TEST(AuthServiceLoginTest, LoginAfterRegisterIssuesEquivalentUserIdentity) {
    // 注册后立刻登录 —— 两次的 user_id / is_admin 必须一致
    LoginFixture f;
    auto u = f.repo->find_by_username("alice");
    ASSERT_TRUE(u.has_value());
    auto r = f.svc->login_user("alice", "password123");
    EXPECT_EQ(r.user_id,  u->id);
    EXPECT_EQ(r.is_admin, u->is_admin);
}

TEST(AuthServiceLoginTest, LoginIsCaseSensitive) {
    // SPEC §2.1 用户名唯一，登录不区分大小写属于"业务策略"问题；
    // 本实现保持存储原样、按 byte 比对，区分大小写。这里固化该行为。
    LoginFixture f;
    EXPECT_THROW(f.svc->login_user("ALICE", "password123"), LoginError);
    EXPECT_NO_THROW (f.svc->login_user("alice", "password123"));
}

// ===========================================================================
//  refresh_access()  ——  SPEC §2.1 POST /api/auth/refresh
// ===========================================================================

// 准备一个含一个用户的 service；外加一个独立的 jwt（用于伪造 token）
struct RefreshFixture {
    std::shared_ptr<InMemoryUserRepo> repo = std::make_shared<InMemoryUserRepo>();
    std::shared_ptr<JwtService>       jwt = std::make_shared<JwtService>(make_jwt_cfg());
    std::shared_ptr<AuthService>      svc;
    oj::domain::RegisterResult        reg;
    RefreshFixture() {
        auto h = std::make_shared<PasswordHasher>();
        svc = std::make_shared<AuthService>(repo, h, jwt);
        reg = svc->register_user("alice", "alice@x.com", "password123");
    }
};

// ---------------------------------------------------------------------------
//  输入校验
// ---------------------------------------------------------------------------
TEST(AuthServiceRefreshTest, RejectsEmptyToken) {
    RefreshFixture f;
    try {
        f.svc->refresh_access("");
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::BadRequest);
    }
}

// ---------------------------------------------------------------------------
//  token 本身无效 —— 签名错 / 篡改 / type 错 / 过期 → 统一 Unauthorized
// ---------------------------------------------------------------------------
TEST(AuthServiceRefreshTest, RejectsMalformedToken) {
    RefreshFixture f;
    try {
        f.svc->refresh_access("not.a.real.jwt");
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::Unauthorized);
    }
}

TEST(AuthServiceRefreshTest, RejectsAccessTokenAsRefresh) {
    // 用 access token 当 refresh —— type 不匹配
    RefreshFixture f;
    auto access = f.jwt->issue_access(1, true);
    try {
        f.svc->refresh_access(access);
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::Unauthorized);
    }
}

TEST(AuthServiceRefreshTest, RejectsTokenSignedWithDifferentSecret) {
    RefreshFixture f;
    JwtConfig bad = make_jwt_cfg();
    bad.secret    = "a-different-32-byte-secret-pad-pad";
    JwtService bad_jwt{bad};
    const auto fake_refresh = bad_jwt.issue_refresh(1);
    try {
        f.svc->refresh_access(fake_refresh);
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::Unauthorized);
    }
}

TEST(AuthServiceRefreshTest, RejectsExpiredToken) {
    JwtConfig short_cfg = make_jwt_cfg();
    short_cfg.refresh_ttl_sec = 1;  // 1s 过期
    auto short_jwt = std::make_shared<JwtService>(short_cfg);
    auto repo  = std::make_shared<InMemoryUserRepo>();
    auto hasher = std::make_shared<PasswordHasher>();
    auto svc   = std::make_shared<AuthService>(repo, hasher, short_jwt);
    auto reg   = svc->register_user("alice", "a@x.com", "password123");

    // 睡到 leeway(5s) 之外
    std::this_thread::sleep_for(std::chrono::seconds(7));
    try {
        svc->refresh_access(reg.refresh_token);
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::Unauthorized);
    }
}

TEST(AuthServiceRefreshTest, RejectsTokenWhoseUserWasDeleted) {
    // "refresh 有效但 DB 里 user 没了"分支：抹掉 repo 里的 user 后再用其
    // 旧 refresh 调 service，必须拒绝并返回 1002。
    RefreshFixture f;
    auto t = f.reg.refresh_token;
    f.repo->remove_for_test(f.reg.user_id);
    try {
        f.svc->refresh_access(t);
        FAIL() << "expected RefreshError";
    } catch (const RefreshError& e) {
        EXPECT_EQ(e.kind(), RefreshErrorKind::Unauthorized);
    }
}

// ---------------------------------------------------------------------------
//  Happy path
// ---------------------------------------------------------------------------
TEST(AuthServiceRefreshTest, ValidRefreshReturnsNewAccessAndNewRefresh) {
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    EXPECT_EQ(r.user_id, f.reg.user_id);
    EXPECT_EQ(r.is_admin, f.reg.is_admin);
    EXPECT_FALSE(r.access_token.empty());
    EXPECT_FALSE(r.refresh_token.empty());
}

TEST(AuthServiceRefreshTest, RefreshTokenIsRotated) {
    // SPEC §2.1：静默刷新会轮换 refresh token
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    EXPECT_NE(r.refresh_token, f.reg.refresh_token)
        << "refresh token must be rotated on each refresh";
    EXPECT_NE(r.access_token, f.reg.access_token)
        << "access token must be different (new iat/exp)";
}

TEST(AuthServiceRefreshTest, OldRefreshTokenIsStillValidInV1NoBlacklist) {
    // v1 没有 refresh 黑名单 —— 旧 refresh 验证仍通过（靠 exp 自然过期）
    // 这是已知行为；后续接黑名单后此测试需更新。
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    auto jwt2 = std::make_shared<JwtService>(make_jwt_cfg());
    EXPECT_NO_THROW(jwt2->verify(r.refresh_token, "refresh"));
    // 旧 refresh 也能 verify 通过（说明它本身仍是合法 JWT）
    EXPECT_NO_THROW(jwt2->verify(f.reg.refresh_token, "refresh"));
}

TEST(AuthServiceRefreshTest, RefreshedAccessTokenHasCurrentIsAdminClaim) {
    // 新 access token 的 adm claim 必须反映**当前** repo 里的 is_admin
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    auto claims = f.jwt->verify(r.access_token, "access");
    EXPECT_EQ(claims.is_admin, f.reg.is_admin);
}

TEST(AuthServiceRefreshTest, RefreshIssuedAccessTokenVerifiesAsAccessType) {
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    auto claims = f.jwt->verify(r.access_token, "access");
    EXPECT_EQ(claims.user_id, r.user_id);
    EXPECT_EQ(claims.type,    "access");
}

TEST(AuthServiceRefreshTest, RefreshIssuedRefreshTokenVerifiesAsRefreshType) {
    RefreshFixture f;
    auto r = f.svc->refresh_access(f.reg.refresh_token);
    auto claims = f.jwt->verify(r.refresh_token, "refresh");
    EXPECT_EQ(claims.user_id, r.user_id);
    EXPECT_EQ(claims.type,    "refresh");
    EXPECT_FALSE(claims.is_admin);  // refresh 不带 adm
}

TEST(AuthServiceRefreshTest, RefreshIsAdminFlagMatchesCurrentRepo) {
    // 第二个 user 走 refresh：is_admin 仍是 false
    auto repo  = std::make_shared<InMemoryUserRepo>();
    auto hasher = std::make_shared<PasswordHasher>();
    auto jwt   = std::make_shared<JwtService>(make_jwt_cfg());
    auto svc   = std::make_shared<AuthService>(repo, hasher, jwt);
    svc->register_user("alice", "a@x.com", "password123");  // admin
    auto bob = svc->register_user("bob", "b@x.com", "password456");  // 非 admin
    auto r = svc->refresh_access(bob.refresh_token);
    EXPECT_FALSE(r.is_admin);
    EXPECT_EQ(r.user_id, bob.user_id);
}

TEST(AuthServiceRefreshTest, ChainOfRefreshesAllStayValid) {
    // 连续多次 refresh 出来的 token 仍可继续 refresh
    RefreshFixture f;
    auto t = f.reg.refresh_token;
    for (int i = 0; i < 3; ++i) {
        auto r = f.svc->refresh_access(t);
        t = r.refresh_token;
        EXPECT_FALSE(r.access_token.empty());
    }
}

}  // namespace
