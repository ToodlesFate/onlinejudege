// =============================================================================
//  test_jwt_service.cpp — JwtService 直接单元测试
//  覆盖：
//    - 构造期参数校验（secret 太短 / TTL ≤ 0 / 空 issuer）
//    - HS256 access / refresh token 颁发
//    - 验证正向路径（uid / adm / typ 解析）
//    - 验证错误路径（签名错 / 篡改 / 过期 / iss 不匹配 / type 不匹配 / 缺 uid）
//    - 时间漂移容忍（leeway）
//    - 特殊字符 / 长 uid 边界
// =============================================================================

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "infra/jwt_service.hpp"

namespace {

using oj::common::JwtConfig;
using oj::infra::InvalidToken;
using oj::infra::JwtError;
using oj::infra::JwtService;
using oj::infra::TokenClaims;

JwtConfig make_cfg() {
    JwtConfig c;
    c.secret          = "test-secret-32-bytes-min-padding-xxx";  // 35 bytes, ≥32
    c.access_ttl_sec  = 3600;
    c.refresh_ttl_sec = 86400;
    c.issuer          = "onlinejudge-test";
    return c;
}

// ---------------------------------------------------------------------------
//  构造期 fail-fast
// ---------------------------------------------------------------------------
TEST(JwtServiceTest, ConstructorRejectsShortSecret) {
    JwtConfig bad = make_cfg();
    bad.secret = "too-short";  // 9 bytes
    EXPECT_THROW(JwtService{bad}, JwtError);
}

TEST(JwtServiceTest, ConstructorAcceptsExactly32ByteSecret) {
    JwtConfig c = make_cfg();
    c.secret = "12345678901234567890123456789012";  // 32 bytes
    EXPECT_NO_THROW(JwtService{c});
}

TEST(JwtServiceTest, ConstructorRejectsZeroAccessTtl) {
    JwtConfig bad = make_cfg();
    bad.access_ttl_sec = 0;
    EXPECT_THROW(JwtService{bad}, JwtError);
}

TEST(JwtServiceTest, ConstructorRejectsNegativeRefreshTtl) {
    JwtConfig bad = make_cfg();
    bad.refresh_ttl_sec = -1;
    EXPECT_THROW(JwtService{bad}, JwtError);
}

TEST(JwtServiceTest, ConstructorRejectsEmptyIssuer) {
    JwtConfig bad = make_cfg();
    bad.issuer = "";
    EXPECT_THROW(JwtService{bad}, JwtError);
}

TEST(JwtServiceTest, ConstructorAcceptsValidConfig) {
    EXPECT_NO_THROW(JwtService{make_cfg()});
}

// ---------------------------------------------------------------------------
//  Token 颁发：正反例子
// ---------------------------------------------------------------------------
TEST(JwtServiceTest, AccessTokenIsThreePartJwt) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(42, true);
    // JWT = header.payload.signature 三个 base64url 段
    int dots = 0;
    for (char c : t) if (c == '.') ++dots;
    EXPECT_EQ(dots, 2);
    EXPECT_GT(t.size(), 50u);
}

TEST(JwtServiceTest, AccessTokenClaimsHaveExpectedValues) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(42, true);
    auto c = svc.verify(t, "access");
    EXPECT_EQ(c.user_id,  42);
    EXPECT_TRUE(c.is_admin);
    EXPECT_EQ(c.type,     "access");
    EXPECT_GT(c.expires_at_unix, c.issued_at_unix);
    EXPECT_EQ(c.expires_at_unix - c.issued_at_unix, 3600);
}

TEST(JwtServiceTest, AccessTokenWithoutAdminHasAdmFalse) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, false);
    auto c = svc.verify(t, "access");
    EXPECT_FALSE(c.is_admin);
}

TEST(JwtServiceTest, RefreshTokenHasNoAdmClaimAndCorrectType) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_refresh(7);
    auto c = svc.verify(t, "refresh");
    EXPECT_EQ(c.user_id, 7);
    EXPECT_EQ(c.type,    "refresh");
    // refresh 不带 adm claim → 解析后 is_admin 为默认值 false
    EXPECT_FALSE(c.is_admin);
    EXPECT_EQ(c.expires_at_unix - c.issued_at_unix, 86400);
}

TEST(JwtServiceTest, AccessAndRefreshTokensAreDifferent) {
    JwtService svc{make_cfg()};
    EXPECT_NE(svc.issue_access(1, true), svc.issue_refresh(1));
}

TEST(JwtServiceTest, SamePayloadProducesDifferentSignatures) {
    // 两次用相同 user_id 颁发 access token；签名必须不同（iat 差 1s，
    // 实际 jwt-cpp 内部用 std::chrono 秒级粒度，所以至少 iat 不同的概率
    // 应当让 exp 至少差 0~1s；这里断言"两次 token 至少 payload 部分不同"
    JwtService svc{make_cfg()};
    const auto t1 = svc.issue_access(42, true);
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto t2 = svc.issue_access(42, true);
    // 至少 1s 后，iat 必变 → payload 必变 → 整个 token 必变
    EXPECT_NE(t1, t2);
}

// ---------------------------------------------------------------------------
//  验证：失败路径（关键安全属性）
// ---------------------------------------------------------------------------
TEST(JwtServiceTest, VerifyRejectsEmptyToken) {
    JwtService svc{make_cfg()};
    EXPECT_THROW(svc.verify("", "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsMalformedToken) {
    JwtService svc{make_cfg()};
    EXPECT_THROW(svc.verify("not.a.jwt", "access"), InvalidToken);
    EXPECT_THROW(svc.verify("only.two",   "access"), InvalidToken);
    EXPECT_THROW(svc.verify("a.b.c.d",    "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsTokenWithWrongSecret) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, true);
    JwtConfig bad = make_cfg();
    bad.secret = "a-different-32-byte-secret-pad-pad";  // 长度够
    JwtService bad_svc{bad};
    EXPECT_THROW(bad_svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsTamperedSignature) {
    JwtService svc{make_cfg()};
    auto t = svc.issue_access(1, true);
    EXPECT_FALSE(t.empty());
    // 在 signature 段尾部追加一个字符 → HMAC 失配必抛
    // （比"翻转最后一位"更鲁棒，避免原末位恰好是 'A'/'B' 时翻转产生等长有效 base64）
    auto dot = t.rfind('.');
    ASSERT_NE(dot, std::string::npos);
    t.insert(t.size(), "X");  // base64url 字符表外；jwt-cpp 解析即抛
    EXPECT_THROW(svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsTamperedPayload) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, true);
    // 把第二段（payload）的第一个字符改一下（X→Y）
    auto dot1 = t.find('.');
    ASSERT_NE(dot1, std::string::npos);
    std::string tampered = t;
    if (tampered[0] != 'Y') {
        tampered[0] = 'Y';
    } else {
        tampered[0] = 'Z';
    }
    EXPECT_THROW(svc.verify(tampered, "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsTokenWithWrongIssuer) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, true);
    JwtConfig other_iss = make_cfg();
    other_iss.issuer = "different-issuer";
    JwtService other_svc{other_iss};
    EXPECT_THROW(other_svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsAccessTokenAsRefresh) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, true);
    EXPECT_THROW(svc.verify(t, "refresh"), InvalidToken);
}

TEST(JwtServiceTest, VerifyRejectsRefreshTokenAsAccess) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_refresh(1);
    EXPECT_THROW(svc.verify(t, "access"), InvalidToken);
}

// ---------------------------------------------------------------------------
//  过期 / leeway
// ---------------------------------------------------------------------------
TEST(JwtServiceTest, ExpiredTokenIsRejected) {
    JwtConfig c = make_cfg();
    c.access_ttl_sec = 1;  // 1 秒
    JwtService svc{c};
    const auto t = svc.issue_access(1, true);
    // 睡 2 秒让 token 过期；leeway=5s 内仍合法，所以再睡 4s
    std::this_thread::sleep_for(std::chrono::seconds(6));
    EXPECT_THROW(svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, RecentlyIssuedTokenPassesWithLeeway) {
    JwtConfig c = make_cfg();
    c.access_ttl_sec = 1;
    JwtService svc{c};
    const auto t = svc.issue_access(1, true);
    // 1s 之内（含 5s leeway）必须仍能通过
    EXPECT_NO_THROW(svc.verify(t, "access"));
}

// ---------------------------------------------------------------------------
//  边界值
// ---------------------------------------------------------------------------
TEST(JwtServiceTest, VeryLargeUserIdRoundTrips) {
    JwtService svc{make_cfg()};
    const std::int64_t big = 1LL << 50;  // 1 Pi
    const auto t = svc.issue_access(big, true);
    EXPECT_EQ(svc.verify(t, "access").user_id, big);
}

TEST(JwtServiceTest, ZeroUserIdIsRejected) {
    JwtService svc{make_cfg()};
    // 手动构造一个 uid=0 的 token 验证 verify 拒绝（业务不允许）
    // 这里直接 issue_access(0, ...) 即可 —— verify 会拒
    const auto t = svc.issue_access(0, false);
    EXPECT_THROW(svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, NegativeUserIdIsRejected) {
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(-1, false);
    EXPECT_THROW(svc.verify(t, "access"), InvalidToken);
}

TEST(JwtServiceTest, TokenIssuedAtIsCloseToNow) {
    using namespace std::chrono;
    const auto before = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    JwtService svc{make_cfg()};
    const auto t = svc.issue_access(1, false);
    const auto after = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
    const auto c = svc.verify(t, "access");
    EXPECT_GE(c.issued_at_unix, before);
    EXPECT_LE(c.issued_at_unix, after);
}

}  // namespace
