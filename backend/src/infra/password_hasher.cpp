#include "infra/password_hasher.hpp"

#include <argon2.h>

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace oj::infra {

namespace {

// 防御性参数校验 —— 任何越界都直接拒绝构造（fail-fast），绝不带病上岗。
// libargon2 自己的 validate_inputs() 也做类似检查，但抛出的错误信息
// 不便直接吞下；在构造阶段先做一次，避免运行时才报错。
//
// memory_cost_kib 的上界：libargon2 64-bit 下理论上限是 4 GiB，但 SPEC §3.2.3
// 的题目 memory_limit_mb 最大 1024 MiB（= 1 GiB），password 哈希不需要那么高；
// 这里取 4 GiB（4 * 1024 * 1024 KiB）作为防御性硬上限，避开 32 位 unsigned overflow。
void validate_params(const PasswordHasher::Params& p) {
    constexpr std::uint32_t MAX_MEMORY_KIB = 4u * 1024u * 1024u;  // 4 GiB
    if (p.time_cost < ARGON2_MIN_TIME || p.time_cost > 1'000'000u) {
        throw HashError("PasswordHasher: time_cost out of range");
    }
    if (p.memory_cost_kib < ARGON2_MIN_MEMORY || p.memory_cost_kib > MAX_MEMORY_KIB) {
        throw HashError("PasswordHasher: memory_cost_kib out of range");
    }
    if (p.parallelism < ARGON2_MIN_LANES || p.parallelism > ARGON2_MAX_LANES) {
        throw HashError("PasswordHasher: parallelism out of range");
    }
    if (p.salt_len < ARGON2_MIN_SALT_LENGTH || p.salt_len > 64u) {
        throw HashError("PasswordHasher: salt_len out of range (must be in [16, 64])");
    }
    if (p.hash_len < ARGON2_MIN_OUTLEN || p.hash_len > 64u) {
        throw HashError("PasswordHasher: hash_len out of range (must be in [4, 64])");
    }
}

// 从 OpenSSL CSPRNG 取 salt；任何失败转 HashError。
std::vector<unsigned char> make_salt(std::uint32_t len) {
    std::vector<unsigned char> salt(len);
    if (RAND_bytes(salt.data(), static_cast<int>(len)) != 1) {
        throw HashError("PasswordHasher: RAND_bytes failed for salt");
    }
    return salt;
}

}  // namespace

PasswordHasher::PasswordHasher() : PasswordHasher(Params{}) {}

PasswordHasher::PasswordHasher(Params params) : params_(params) {
    validate_params(params_);
}

std::size_t PasswordHasher::encoded_len_upper_bound() const noexcept {
    // argon2_encodedlen 返回包含 '\0' 的所需字节数
    const std::size_t n = argon2_encodedlen(
        params_.time_cost,
        params_.memory_cost_kib,
        params_.parallelism,
        params_.salt_len,
        params_.hash_len,
        Argon2_id);
    // 减去 '\0'；保留一点点冗余做 assert 校验
    return n > 0 ? n - 1 : 0;
}

std::string PasswordHasher::hash(std::string_view password) const {
    // 1) 生成随机盐
    auto salt = make_salt(params_.salt_len);

    // 2) 计算 PHC 编码所需 buffer 长度并分配（+1 给 '\0'）
    const std::size_t enc_len = encoded_len_upper_bound();
    std::string encoded(enc_len + 1, '\0');

    // 3) 调用 libargon2 一次性产出 PHC 字符串（内含 base64(salt) + base64(hash)）
    const int rc = argon2id_hash_encoded(
        params_.time_cost,
        params_.memory_cost_kib,
        params_.parallelism,
        password.data(), password.size(),
        salt.data(),        salt.size(),
        params_.hash_len,
        encoded.data(),     encoded.size());

    if (rc != ARGON2_OK) {
        throw HashError(std::string{"PasswordHasher: argon2id_hash_encoded failed: "}
                        + argon2_error_message(rc));
    }

    // 4) 去掉尾部 '\0'，构造 std::string 返回
    const std::size_t real_len = std::strlen(encoded.c_str());
    encoded.resize(real_len);
    return encoded;
}

bool PasswordHasher::verify(std::string_view password,
                            std::string_view encoded) const noexcept {
    // 任何空 / 过短输入直接拒绝（仅拒绝 encoded 空 —— 业务上不会 hash 空密码，
    // 但作为纯 crypto 单元我们依然允许 verify("", hash(""))，让 libargon2 自己判断）
    if (encoded.empty()) {
        return false;
    }
    // 长度上限防御：防止异常输入撑爆 libargon2 内部 buffer
    if (encoded.size() > 1024) {
        return false;
    }

    // libargon2 接受 const char*；构造一个带 '\0' 的临时拷贝
    std::string enc_str{encoded};
    const int rc = argon2id_verify(
        enc_str.c_str(),
        password.data(), password.size());

    // argon2id_verify 返回值：ARGON2_OK (=0) 表匹配；其他任何值（含
    // VERIFY_MISMATCH）一律视为不匹配。绝不向调用方泄漏错误码。
    return rc == ARGON2_OK;
}

bool PasswordHasher::is_encoded_hash(std::string_view s) noexcept {
    if (s.empty() || s.size() < 32) {
        return false;
    }
    // 必须以 "$argon2id$" / "$argon2i$" / "$argon2d$" 开头
    constexpr std::string_view prefixes[] = {
        "$argon2id$",
        "$argon2i$",
        "$argon2d$",
    };
    bool ok_prefix = false;
    for (auto p : prefixes) {
        if (s.substr(0, p.size()) == p) {
            ok_prefix = true;
            break;
        }
    }
    if (!ok_prefix) return false;

    // PHC 格式必须至少有 5 段 $ 分隔：type / version / params / salt / hash
    int dollar_count = 0;
    for (char c : s) {
        if (c == '$') ++dollar_count;
        if (dollar_count >= 5) return true;
    }
    return false;
}

std::chrono::milliseconds
PasswordHasher::benchmark(std::string_view sample_password) const {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    (void)hash(sample_password);  // 丢弃结果，仅测耗时
    const auto t1 = clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
}

}  // namespace oj::infra