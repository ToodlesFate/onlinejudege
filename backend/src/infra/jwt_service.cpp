#include "infra/jwt_service.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <sstream>
#include <utility>

#include <jwt-cpp/jwt.h>
#include <picojson/picojson.h>

namespace oj::infra {

namespace {

constexpr std::string_view kAccessType  = "access";
constexpr std::string_view kRefreshType = "refresh";

// 把 time_point 转换为 unix 秒
std::int64_t to_unix(std::chrono::system_clock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::seconds>(
        tp.time_since_epoch()).count();
}

// 生成 128-bit 随机 token id —— 每次 issue 都新生成，让 token 真正"轮换"
// （JWT iat 是秒级粒度，同一秒内签两次 token 内容会完全相同；加 jti 后
// 即使 iat 相同 payload 也不同，refresh 才能产生不同 token）
std::string new_jti() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uint64_t a = rng();
    std::uint64_t b = rng();
    std::ostringstream oss;
    oss << std::hex << a << b;
    return oss.str();
}

// 包装 int64_t 为 picojson::value —— 用赋值 + 局部变量避免
// `picojson::value(int64_t{x})` 模板构造歧义
picojson::value value_of_int64(std::int64_t v) {
    picojson::value out;
    out.set<int64_t>(v);
    return out;
}

}  // namespace

JwtService::JwtService(common::JwtConfig cfg) : cfg_(std::move(cfg)) {
    // SPEC §3.2.3: secret must be ≥ 32 字节随机
    if (cfg_.secret.size() < 32) {
        throw JwtError("JwtService: secret must be >= 32 bytes, got " +
                       std::to_string(cfg_.secret.size()));
    }
    if (cfg_.access_ttl_sec <= 0 || cfg_.refresh_ttl_sec <= 0) {
        throw JwtError("JwtService: access/refresh TTL must be > 0");
    }
    if (cfg_.issuer.empty()) {
        throw JwtError("JwtService: issuer must not be empty");
    }
}

std::string JwtService::issue_access(std::int64_t user_id, bool is_admin) const {
    const auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_type("JWT")
        .set_issuer(cfg_.issuer)
        .set_id(new_jti())  // 唯一 token id，让每次签发都有不同的 payload
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(cfg_.access_ttl_sec))
        .set_payload_claim("uid", jwt::claim(value_of_int64(user_id)))
        .set_payload_claim("adm", jwt::claim(picojson::value(is_admin)))
        .set_payload_claim("typ", jwt::claim(std::string{kAccessType}))
        .sign(jwt::algorithm::hs256{cfg_.secret});
}

std::string JwtService::issue_refresh(std::int64_t user_id) const {
    const auto now = std::chrono::system_clock::now();
    return jwt::create()
        .set_type("JWT")
        .set_issuer(cfg_.issuer)
        .set_id(new_jti())  // 唯一 token id，refresh 才能真正"轮换"
        .set_issued_at(now)
        .set_expires_at(now + std::chrono::seconds(cfg_.refresh_ttl_sec))
        .set_payload_claim("uid", jwt::claim(value_of_int64(user_id)))
        .set_payload_claim("typ", jwt::claim(std::string{kRefreshType}))
        .sign(jwt::algorithm::hs256{cfg_.secret});
}

TokenClaims JwtService::verify(std::string_view token,
                               std::string_view expected_type) const {
    try {
        const auto decoded = jwt::decode(std::string{token});
        const auto verifier = jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{cfg_.secret})
            .with_issuer(cfg_.issuer)
            .leeway(5);  // 5s 时钟漂移容忍
        verifier.verify(decoded);

        TokenClaims c;
        if (decoded.has_payload_claim("uid")) {
            const auto& v = decoded.get_payload_claim("uid").to_json();
            if (v.is<int64_t>()) c.user_id = v.get<int64_t>();
        }
        if (decoded.has_payload_claim("adm")) {
            const auto& v = decoded.get_payload_claim("adm").to_json();
            if (v.is<bool>()) c.is_admin = v.get<bool>();
        }
        if (decoded.has_payload_claim("typ")) {
            c.type = decoded.get_payload_claim("typ").as_string();
        }
        if (decoded.has_issued_at()) {
            c.issued_at_unix = to_unix(decoded.get_issued_at());
        }
        if (decoded.has_expires_at()) {
            c.expires_at_unix = to_unix(decoded.get_expires_at());
        }
        if (decoded.has_id()) {
            c.jti = decoded.get_id();
        }

        if (c.type != expected_type) {
            throw InvalidToken("token type mismatch: expected " +
                               std::string{expected_type} + " got " + c.type);
        }
        if (c.user_id <= 0) {
            throw InvalidToken("token missing or invalid uid claim");
        }
        return c;
    } catch (const InvalidToken&) {
        throw;
    } catch (const std::exception& e) {
        throw InvalidToken(std::string{"verify failed: "} + e.what());
    }
}

}  // namespace oj::infra
