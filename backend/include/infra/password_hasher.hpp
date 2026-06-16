#pragma once

// =============================================================================
//  oj::infra::PasswordHasher — Argon2id 密码哈希模块
//  SPEC §2.1：密码哈希 Argon2id（argon2 库）
//  SPEC §9.3 S-2：密码以 Argon2id 存储，DB 中无可逆值
//
//  设计要点：
//    1. 仅依赖 libargon2 (system pkg) + OpenSSL (RAND_bytes, 已链接)
//    2. 默认参数遵循 OWASP Password Storage Cheat Sheet (2024) 对 Argon2id
//       的推荐：t=3, m=64 MiB, p=4, salt=16 B, hash=32 B
//    3. 输出为标准 PHC 编码字符串（libargon2 默认格式），便于以后升级
//       调参或换库时无需迁移数据
//    4. verify() 是 const + noexcept 的纯函数，行为对错误密码/篡改哈希/
//       格式异常的输入均返回 false，绝不抛异常（登录路径容错关键）
//    5. hash() 在 RNG / libargon2 失败时抛 HashError，由调用方决定
//       是 5xx 还是 fallback
// =============================================================================

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oj::infra {

// hash() 失败时抛出的异常类型 —— 由调用方（AuthService/UserRepo）转 ErrorCode
class HashError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 无状态、线程安全的 Argon2id hasher。
// 典型用法：
//     oj::infra::PasswordHasher hasher;
//     auto encoded = hasher.hash(plain);     // 写入 DB password_hash
//     bool ok      = hasher.verify(plain, encoded);
class PasswordHasher {
public:
    // 调参集合 —— 默认值选 OWASP 2024 推荐 + SPEC §3.2.3 默认 memory=256 MiB
    // 的 1/4 以兼顾登录吞吐（SPEC §2.6 性能：提交/登录 P95 < 200ms，
    // 单次 64 MiB / t=3 在典型 CPU 上约 30–80 ms，登录路径可承受）。
    struct Params {
        std::uint32_t time_cost       = 3;            // t（迭代轮数）
        std::uint32_t memory_cost_kib = 64u * 1024u;  // m = 64 MiB
        std::uint32_t parallelism     = 4;            // p（并行 lanes）
        std::uint32_t salt_len        = 16;           // 盐长度（字节）
        std::uint32_t hash_len        = 32;           // 摘要长度（字节）
    };

    // 构造时给定调参；不带参数则使用 Params{} 默认值（OWASP 推荐基线）。
    // 注意：不能用 `Params params = Params{}` 这种语法 —— 默认成员初始化
    // 在外层类未结束前不可见。改用两个重载解决。
    PasswordHasher();

    explicit PasswordHasher(Params params);

    [[nodiscard]] Params params() const noexcept { return params_; }

    // 算法名常量 —— 用于日志 / 指标 / 文档
    [[nodiscard]] constexpr static std::string_view algorithm() noexcept {
        return "argon2id";
    }

    // 计算 PHC 编码哈希字符串，格式：
    //     $argon2id$v=19$m=<KiB>,t=<iter>,p=<lanes>$<base64-salt>$<base64-hash>
    // 失败时抛 HashError（RNG 失败 / 参数非法 / libargon2 内部错误）。
    [[nodiscard]] std::string hash(std::string_view password) const;

    // 校验 password 是否匹配已编码哈希。
    // 任何异常路径（空串 / 格式错 / 篡改 / 密码不符）均返回 false，
    // 函数本身绝不抛异常 —— 登录路径不能因校验失败导致进程崩溃。
    [[nodiscard]] bool verify(std::string_view password,
                              std::string_view encoded) const noexcept;

    // 启发式判断 s 是否像 Argon2 PHC 编码串：
    //    - 必须以 "$argon2id$" 或 "$argon2i$" 或 "$argon2d$" 开头
    //    - 包含至少 4 段 $ 分隔的子串（type/version/params/salt/hash）
    // 用于 DB 升级期数据迁移时识别"明文 / 老旧 hash / argon2id" 行。
    [[nodiscard]] static bool is_encoded_hash(std::string_view s) noexcept;

    // 调参辅助：用 sample 跑一次 hash 并测耗时，便于在不同硬件上调出
    // 目标 ~100 ms 的 cost。不在生产热路径调用，仅供离线基准测试。
    [[nodiscard]] std::chrono::milliseconds
    benchmark(std::string_view sample_password) const;

    // PHC 编码后大致长度（不含 '\0'）。可用于评估 password_hash 列
    // VARCHAR(255) 是否够用 —— 现参数下约 96 字节，远小于 255。
    [[nodiscard]] std::size_t encoded_len_upper_bound() const noexcept;

private:
    Params params_;
};

}  // namespace oj::infra