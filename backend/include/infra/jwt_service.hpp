#pragma once

// =============================================================================
//  oj::infra::JwtService — HS256 JWT 颁发 / 验证
//  SPEC §2.1 / §3.2.2 "JwtService" / §5.2.1
//  Access Token TTL=2h，Refresh Token TTL=7d；claim 含 user_id / is_admin / type
//
//  设计要点：
//    1. HS256 对称签名：secret 必须 ≥ 32 字节随机（构造期 fail-fast）
//    2. claim 使用 jwt-cpp 默认 traits（picojson）
//    3. issue() 不抛异常（除 JwtError 表示配置/系统错误）；verify 失败抛 InvalidToken
//    4. 验证时强制要求 iss / exp / 签名一致；type 字段区分 access/refresh
// =============================================================================

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "common/config.hpp"

namespace oj::infra {

class JwtError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InvalidToken : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 验证后返回的 claim 快照
struct TokenClaims {
    std::int64_t user_id{0};
    bool         is_admin{false};
    std::string  type;        // "access" / "refresh"
    std::int64_t issued_at_unix{0};
    std::int64_t expires_at_unix{0};
};

class JwtService {
public:
    // 构造期校验：secret ≥ 32 字节、TTL > 0
    explicit JwtService(common::JwtConfig cfg);
    ~JwtService() = default;

    JwtService(const JwtService&)            = delete;
    JwtService& operator=(const JwtService&) = delete;

    // 颁发 access token
    [[nodiscard]] std::string issue_access(std::int64_t user_id, bool is_admin) const;

    // 颁发 refresh token
    [[nodiscard]] std::string issue_refresh(std::int64_t user_id) const;

    // 解析 + 校验；签名错 / 过期 / iss 不匹配 / type 不匹配 → 抛 InvalidToken
    [[nodiscard]] TokenClaims verify(std::string_view token,
                                     std::string_view expected_type) const;

    // refresh token 的有效期（秒），供 Set-Cookie 的 Max-Age 用
    [[nodiscard]] int refresh_ttl_sec() const noexcept { return cfg_.refresh_ttl_sec; }

    [[nodiscard]] const common::JwtConfig& config() const noexcept { return cfg_; }

private:
    common::JwtConfig cfg_;
};

}  // namespace oj::infra
