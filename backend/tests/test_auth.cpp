// =============================================================================
//  test_auth.cpp — 阶段 2 账户系统单元测试
//    覆盖 PasswordHasher（Argon2id 哈希 + 校验）全部关键路径
//
//  依据 SPEC §3.2.1：
//      tests/test_auth.cpp ← AuthService / JwtService / PasswordHasher
//
//  本阶段只覆盖 PasswordHasher；后续阶段再补 AuthService / JwtService。
//
//  测试目标（SPEC §9.3 S-2 "密码以 Argon2id 存储，DB 中无可逆值"）：
//    1. PHC 编码格式合规（$argon2id$v=19$m=...,t=...,p=...$salt$hash）
//    2. 同一密码两次 hash 输出不同（盐随机性）
//    3. verify() 对正确密码返回 true
//    4. verify() 对错误密码 / 空密码 / 篡改 hash / 空 encoded / 异常格式
//       均返回 false，绝不抛异常
//    5. is_encoded_hash() 启发式判断正确
//    6. 构造期参数校验（fail-fast）
//    7. encoded 长度上限与 DB VARCHAR(255) 兼容
//    8. 非 ASCII 密码（如中文）正常工作
// =============================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "infra/password_hasher.hpp"

namespace {

using oj::infra::HashError;
using oj::infra::PasswordHasher;

// ---------------------------------------------------------------------------
//  PHC 编码格式
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, HashEmitsPhcEncodedArgon2id) {
    PasswordHasher h;
    const auto enc = h.hash("correct horse battery staple");

    // 1) 必须以 "$argon2id$" 开头
    ASSERT_GE(enc.size(), 30u);
    EXPECT_EQ(enc.substr(0, 10), "$argon2id$");

    // 2) 必须有 5 段 $ 分隔（type / version / params / salt / hash）
    int dollars = 0;
    for (char c : enc) if (c == '$') ++dollars;
    EXPECT_EQ(dollars, 5);

    // 3) version 段必须是 v=19（libargon2 当前稳定版）
    const auto v_pos = enc.find("$v=");
    ASSERT_NE(v_pos, std::string::npos);
    EXPECT_EQ(enc.substr(v_pos, 5), "$v=19");

    // 4) params 段必须包含 m= / t= / p=
    EXPECT_NE(enc.find("m="), std::string::npos);
    EXPECT_NE(enc.find(",t="), std::string::npos);
    EXPECT_NE(enc.find(",p="), std::string::npos);

    // 5) 整个串不含 '\0' 等控制字符
    for (char c : enc) {
        EXPECT_GE(static_cast<unsigned char>(c), 0x20u)
            << "unexpected control byte in encoded hash";
    }
}

TEST(PasswordHasherTest, EncodedLengthFitsVarchar255) {
    // SPEC §4.2 password_hash VARCHAR(255) —— 默认参数下 encoded 不应超过 255。
    PasswordHasher h;
    for (int i = 0; i < 5; ++i) {
        const auto enc = h.hash("some-password");
        EXPECT_LE(enc.size(), 255u)
            << "encoded longer than VARCHAR(255); iteration " << i;
    }
}

TEST(PasswordHasherTest, EncodedLenUpperBoundMatchesActual) {
    // encoded_len_upper_bound() 必须 ≥ 实际任意次 hash 的长度
    PasswordHasher h;
    const auto ub = h.encoded_len_upper_bound();
    EXPECT_GT(ub, 60u) << "encoded length upper bound suspiciously small";

    for (int i = 0; i < 5; ++i) {
        const auto enc = h.hash("x");
        EXPECT_LE(enc.size(), ub)
            << "actual hash exceeded declared upper bound";
    }
}

// ---------------------------------------------------------------------------
//  盐随机性：同一密码两次 hash 输出必须不同
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, SamePasswordProducesDifferentHashes) {
    PasswordHasher h;
    const std::string pw = "SuperSecretPassw0rd!";

    std::vector<std::string> seen;
    for (int i = 0; i < 8; ++i) {
        seen.push_back(h.hash(pw));
    }

    // 任何两条都不相等
    for (std::size_t i = 0; i < seen.size(); ++i) {
        for (std::size_t j = i + 1; j < seen.size(); ++j) {
            EXPECT_NE(seen[i], seen[j])
                << "two hashes collided at i=" << i << " j=" << j;
        }
    }
}

// ---------------------------------------------------------------------------
//  verify() 正确路径
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, VerifyAcceptsCorrectPassword) {
    PasswordHasher h;
    const auto enc = h.hash("hunter2");
    EXPECT_TRUE(h.verify("hunter2", enc));
}

TEST(PasswordHasherTest, VerifyAcceptsNonAsciiPassword) {
    PasswordHasher h;
    const std::string pw = "中文密码123";
    const auto enc = h.hash(pw);
    EXPECT_TRUE(h.verify(pw, enc));
    EXPECT_FALSE(h.verify("中文密码124", enc));
}

TEST(PasswordHasherTest, VerifyAcceptsLongPassword) {
    PasswordHasher h;
    // 200 字节密码
    const std::string pw(200, 'a');
    const auto enc = h.hash(pw);
    EXPECT_TRUE(h.verify(pw, enc));
}

TEST(PasswordHasherTest, VerifyAcceptsEmptyEncodedAfterHashing) {
    // 边界：哈希一个空字符串应当也能 verify 回空字符串
    // （业务上不会注册空密码 —— 由 AuthService 在更上层拦截）
    PasswordHasher h;
    const auto enc = h.hash("");
    EXPECT_TRUE(h.verify("", enc));
}

// ---------------------------------------------------------------------------
//  verify() 错误 / 异常路径（关键：必须 noexcept + 仅返回 false）
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, VerifyRejectsWrongPassword) {
    PasswordHasher h;
    const auto enc = h.hash("right");
    EXPECT_FALSE(h.verify("wrong", enc));
    EXPECT_FALSE(h.verify("Right", enc));   // 大小写敏感
    EXPECT_FALSE(h.verify("right ", enc));  // 多一个空格
    EXPECT_FALSE(h.verify(" right", enc));  // 多一个空格
}

TEST(PasswordHasherTest, VerifyRejectsTamperedHash) {
    PasswordHasher h;
    auto enc = h.hash("hunter2");

    // 翻转最后一位（base64 字母表外，argon2 应当返回 mismatch）
    EXPECT_FALSE(enc.empty());
    char& last = enc.back();
    last = (last == 'A') ? 'B' : 'A';
    EXPECT_FALSE(h.verify("hunter2", enc));
}

TEST(PasswordHasherTest, VerifyRejectsTamperedSalt) {
    PasswordHasher h;
    auto enc = h.hash("hunter2");

    // PHC: $argon2id$v=19$m=...,t=...,p=...$<salt>$<hash>
    //  按 '$' 切成 6 段（首尾两个空串）：type / version / params / salt / hash / ""
    std::vector<std::string> parts;
    std::string cur;
    for (char c : enc) {
        if (c == '$') { parts.push_back(std::move(cur)); cur.clear(); }
        else          { cur.push_back(c); }
    }
    parts.push_back(std::move(cur));
    ASSERT_EQ(parts.size(), 6u) << "PHC must split into 6 parts";
    EXPECT_EQ(parts[0], "");          // before first '$'
    EXPECT_EQ(parts[1], "argon2id");
    EXPECT_EQ(parts[2], "v=19");
    EXPECT_NE(parts[3].find("m="),   std::string::npos);
    EXPECT_FALSE(parts[4].empty());   // salt
    EXPECT_FALSE(parts[5].empty());   // hash

    // 改 salt 段的一个字符（替换为合法 base64 字符之外也不算，要保证 tamper 后
    // 字符串仍合法但 hash 失配；最简单：翻转 salt 串的最后一个字符）
    ASSERT_FALSE(parts[4].empty());
    char& s = parts[4].back();
    s = (s == 'A') ? 'B' : 'A';

    // 拼回
    std::string tampered;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) tampered.push_back('$');
        tampered += parts[i];
    }

    EXPECT_FALSE(h.verify("hunter2", tampered))
        << "tampered salt must reject valid password";
}

TEST(PasswordHasherTest, VerifyRejectsTamperedParams) {
    // 把 m=64KiB 改成 m=65KiB —— encoded hash 应当不匹配
    PasswordHasher h;
    auto enc = h.hash("hunter2");
    // "m=65536" 是默认 64 MiB 参数；篡改成 m=99999
    const auto pos = enc.find("m=65536");
    ASSERT_NE(pos, std::string::npos);
    enc.replace(pos, 7, "m=99999");
    // 篡改参数后即使密码正确也应被 libargon2 拒掉
    EXPECT_FALSE(h.verify("hunter2", enc));
}

TEST(PasswordHasherTest, VerifyRejectsEmptyInputs) {
    PasswordHasher h;
    EXPECT_FALSE(h.verify("any", ""));
    EXPECT_FALSE(h.verify("", "$argon2id$v=19$m=65536,t=3,p=4$xxx$yyy"));
    EXPECT_FALSE(h.verify("", ""));
}

TEST(PasswordHasherTest, VerifyRejectsMalformedEncoded) {
    PasswordHasher h;
    EXPECT_FALSE(h.verify("any", "not-a-phc-string"));
    EXPECT_FALSE(h.verify("any", "bcrypt$2a$10$..."));       // 错的算法前缀
    EXPECT_FALSE(h.verify("any", "$argon2id$"));               // 不完整
    EXPECT_FALSE(h.verify("any", "$argon2id$v=19"));           // 缺参数
    EXPECT_FALSE(h.verify("any", "$argon2id$v=19$m=1,t=1,p=1$"));  // 缺 salt+hash
}

TEST(PasswordHasherTest, VerifyRejectsOversizedInput) {
    // 输入 > 1024 字节直接拒掉，防止 libargon2 内部 buffer 异常
    PasswordHasher h;
    const std::string huge(2000, 'x');
    EXPECT_FALSE(h.verify("any", huge));
}

// ---------------------------------------------------------------------------
//  is_encoded_hash 启发式
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, IsEncodedHashAcceptsAllArgon2Variants) {
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=65536,t=3,p=4$c29tZXNhbHQAAAAAAAAAAA$"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(
        "$argon2i$v=19$m=65536,t=3,p=4$xxx$yyy"));
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(
        "$argon2d$v=19$m=65536,t=3,p=4$xxx$yyy"));
}

TEST(PasswordHasherTest, IsEncodedHashRejectsNonPhc) {
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(""));
    EXPECT_FALSE(PasswordHasher::is_encoded_hash("plaintext_password"));
    EXPECT_FALSE(PasswordHasher::is_encoded_hash("bcrypt$2a$10$abcdef"));
    EXPECT_FALSE(PasswordHasher::is_encoded_hash("$argon2id$"));  // 太短
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=65536,t=3,p=4"));                      // 段数不够
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(
        "$argon2x$v=19$m=65536,t=3,p=4$xxx$yyy"));               // 未知变体
}

// ---------------------------------------------------------------------------
//  算法名常量
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, AlgorithmIsArgon2id) {
    EXPECT_EQ(PasswordHasher::algorithm(), "argon2id");
}

// ---------------------------------------------------------------------------
//  构造期参数校验（fail-fast：异常路径在 hash() 之前就被截住）
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, ConstructorRejectsZeroTimeCost) {
    PasswordHasher::Params bad{};
    bad.time_cost = 0;
    EXPECT_THROW(PasswordHasher{bad}, HashError);
}

TEST(PasswordHasherTest, ConstructorRejectsUnderMinSalt) {
    PasswordHasher::Params bad{};
    bad.salt_len = 4;  // libargon2 要求至少 8
    EXPECT_THROW(PasswordHasher{bad}, HashError);
}

TEST(PasswordHasherTest, ConstructorRejectsOverMaxMemory) {
    PasswordHasher::Params bad{};
    bad.memory_cost_kib = 0xFFFFFFFFu;
    EXPECT_THROW(PasswordHasher{bad}, HashError);
}

TEST(PasswordHasherTest, ConstructorAcceptsTunedLowParams) {
    // 测试环境希望 hash() 越快越好（CI 跑全套测试 < 30s），
    // 可以接受 t=1, m=8MiB, p=1 等低配置 —— 必须能构造成功。
    PasswordHasher::Params fast{};
    fast.time_cost       = 1;
    fast.memory_cost_kib = 8 * 1024;
    fast.parallelism     = 1;
    fast.salt_len        = 16;
    fast.hash_len        = 32;
    EXPECT_NO_THROW({
        PasswordHasher h{fast};
        const auto enc = h.hash("pw");
        EXPECT_TRUE(h.verify("pw", enc));
    });
}

// ---------------------------------------------------------------------------
//  调参：不同 Params 实例相互独立、不共享状态
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, DifferentInstancesDoNotShareState) {
    PasswordHasher::Params a{};
    a.memory_cost_kib = 8 * 1024;
    PasswordHasher::Params b{};
    b.memory_cost_kib = 16 * 1024;
    PasswordHasher ha{a}, hb{b};

    EXPECT_EQ(ha.params().memory_cost_kib, 8u * 1024u);
    EXPECT_EQ(hb.params().memory_cost_kib, 16u * 1024u);

    const auto ea = ha.hash("p");
    const auto eb = hb.hash("p");

    // verify 各自能 verify 自己的密码
    EXPECT_TRUE(ha.verify("p", ea));
    EXPECT_TRUE(hb.verify("p", eb));

    // encoded 串里的 m= 参数不同
    EXPECT_NE(ea.find("m=8192"),  std::string::npos);
    EXPECT_NE(eb.find("m=16384"), std::string::npos);
}

// ---------------------------------------------------------------------------
//  benchmark() 至少要返回一个有限时长
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, BenchmarkReturnsNonNegativeDuration) {
    PasswordHasher h;
    const auto dur = h.benchmark("benchmark-input");
    EXPECT_GE(dur.count(), 0);
    // 1 KiB memory + 1 iteration 在任何机器上至少 1ms
    EXPECT_LT(dur, std::chrono::seconds{10})
        << "benchmark took absurdly long; params likely not actually low";
}

// ---------------------------------------------------------------------------
//  性能预算（CI 友好）：默认参数下 1 次 hash + 1 次 verify 总耗时 < 2s
//  说明：默认参数是 t=3, m=64MiB —— 单线程消费约 30–150ms；
//  整轮 1.5s 是宽松 CI 预算。生产环境性能（SPEC §2.6 P95<200ms）
//  留给后续 AuthService / UserRepo 的集成测试验证。
// ---------------------------------------------------------------------------
TEST(PasswordHasherTest, DefaultParamsHashAndVerifyWithinBudget) {
    PasswordHasher h;
    const auto t0 = std::chrono::steady_clock::now();
    const auto enc = h.hash("PerformanceBudgetCheck");
    const bool ok  = h.verify("PerformanceBudgetCheck", enc);
    const auto t1 = std::chrono::steady_clock::now();

    EXPECT_TRUE(ok);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);
    EXPECT_LT(ms.count(), 2000)
        << "default Argon2id params slower than CI budget: " << ms.count() << "ms";
}

// ===========================================================================
//  阶段 2 增强测试 —— 覆盖原 25 项之外的关键缺口：
//
//    A. 线程安全 (concurrent hash + verify from N threads)
//    B. 跨实例可移植性 (一个 hasher 产生的 encoded 可被另一实例 verify)
//    C. 不同 params 产出不同 hash (params 真正参与编码)
//    D. PHC 段级结构 (每段 m=/t=/p= / salt / hash 形态正确)
//    E. Unicode 边界 (emoji / 组合字符 / 控制字符)
//    F. 抗指纹：encoded 串不含明文密码 / 盐字节 / 长前缀
//    G. 参数验证：边界值（最小合法 + 刚好越界）
//    H. is_encoded_hash 段数严格匹配
//    I. Determinism: verify() 是纯函数（无副作用、可重入）
// ===========================================================================

// ---------------------------------------------------------------------------
//  A. 线程安全
// ---------------------------------------------------------------------------
TEST(PasswordHasherConcurrency, ConcurrentHashesProduceUniqueSalts) {
    // 8 个线程各做 5 次 hash，总共 40 次 hash 应当两两不同
    constexpr int kThreads = 8;
    constexpr int kPerThread = 5;

    PasswordHasher h;
    std::mutex mu;
    std::set<std::string> seen;
    std::atomic<int> collisions{0};

    std::vector<std::thread> ts;
    ts.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                const auto enc = h.hash("same-password");
                std::lock_guard<std::mutex> lock(mu);
                if (!seen.insert(enc).second) {
                    ++collisions;
                }
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(collisions.load(), 0)
        << "Argon2id produced duplicate salts across threads";
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(kThreads * kPerThread));
}

TEST(PasswordHasherConcurrency, ConcurrentHashAndVerifyAllSucceed) {
    // 混合读写：4 线程做 hash，4 线程同时对已有 encoded 做 verify
    PasswordHasher h;
    const std::string shared_pw = "threaded-secret";

    std::vector<std::string> encoded_pool;
    for (int i = 0; i < 20; ++i) {
        encoded_pool.push_back(h.hash(shared_pw));
    }

    std::atomic<int> hash_ok{0};
    std::atomic<int> verify_ok{0};
    std::atomic<int> verify_fail{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 5; ++i) {
                const auto enc = h.hash(shared_pw);
                if (h.verify(shared_pw, enc)) ++hash_ok;
            }
        });
    }
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&] {
            for (const auto& enc : encoded_pool) {
                if (h.verify(shared_pw, enc)) ++verify_ok;
                else ++verify_fail;
            }
        });
    }
    for (auto& th : ts) th.join();

    EXPECT_EQ(hash_ok.load(),    20);
    EXPECT_EQ(verify_ok.load(),  4 * 20);
    EXPECT_EQ(verify_fail.load(), 0);
}

TEST(PasswordHasherConcurrency, VerifyIsReentrantAndStateless) {
    // 同一个 PasswordHasher 实例被多线程共享使用，且 hash() 与 verify()
    // 互相穿插 —— 应当互不干扰（内部无共享可变状态）
    PasswordHasher h;
    const std::string pw = "reentrant-pw";

    // 预热：先 hash 一次拿一个 encoded 用于 verify
    const std::string baseline = h.hash(pw);

    std::atomic<int> errors{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 10; ++i) {
                if (t % 2 == 0) {
                    // hash
                    const auto enc = h.hash(pw);
                    if (!h.verify(pw, enc)) ++errors;
                } else {
                    // verify
                    if (!h.verify(pw, baseline)) ++errors;
                }
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
//  B. 跨实例可移植性
// ---------------------------------------------------------------------------
TEST(PasswordHasherPortability, HashFromOneInstanceVerifiesOnAnother) {
    // hasher A 用默认参数 hash 一段密码，
    // hasher B 用完全不同的 params（只要 encoded 里的 m/t/p 被尊重即可）
    // 注意：因为 PHC 字符串自带参数，verify 不依赖 hasher 本身的 params；
    // 但要保证 B 的 params 合法（不能小到拒绝验证任意长度的输入）。
    PasswordHasher::Params a_params{};  // 默认
    PasswordHasher::Params b_params{};
    b_params.time_cost       = 1;
    b_params.memory_cost_kib = 8 * 1024;
    b_params.parallelism     = 1;

    PasswordHasher a{a_params};
    PasswordHasher b{b_params};

    const std::string pw = "portability-test";
    const auto enc = a.hash(pw);

    EXPECT_TRUE(a.verify(pw, enc));
    EXPECT_TRUE(b.verify(pw, enc)) << "cross-instance verify must work";

    // 反向：b 生成的 hash 也能被 a 验证
    const auto enc2 = b.hash(pw);
    EXPECT_TRUE(b.verify(pw, enc2));
    EXPECT_TRUE(a.verify(pw, enc2));
}

TEST(PasswordHasherPortability, EncodedStringIsSelfDescribing) {
    // PHC 自包含 —— 把 encoded 串从 hasher A "拿给" hasher B（哪怕 B 的
    // 当前 params 完全不一样），只要 B 的 params 满足 verify() 的最低资源
    // 要求就能验证。 关键验证点：encoded 串中 m=/t=/p= 三个数字必须独立
    // 于调用 hasher 的 params（否则就破坏了自包含）。
    PasswordHasher::Params fast{};
    fast.time_cost       = 1;
    fast.memory_cost_kib = 8 * 1024;
    fast.parallelism     = 1;

    PasswordHasher h_fast{fast};

    // 用 fast hasher hash 一段密码，看 encoded 是否如实写出 fast params
    const auto enc = h_fast.hash("self-desc");
    EXPECT_NE(enc.find("m=8192"),  std::string::npos)
        << "encoded should carry its own m= parameter";
    EXPECT_NE(enc.find(",t=1"),    std::string::npos)
        << "encoded should carry its own t= parameter";
    EXPECT_NE(enc.find(",p=1"),    std::string::npos)
        << "encoded should carry its own p= parameter";
}

// ---------------------------------------------------------------------------
//  C. 不同 params 产出不同 hash
// ---------------------------------------------------------------------------
TEST(PasswordHasherParamsSemantics, HigherTimeCostYieldsDifferentHash) {
    // 同一密码 + 同一盐下，t 不同 → hash 不同。
    // 我们不能控制盐，但可以并行 hash 很多次，统计"两个 hash 是否曾完全相同"
    // （统计意义上为 0，因为盐随机）。
    // 这里用更弱的断言：t=1 与 t=3 各自的 hash 集合不相交。
    PasswordHasher::Params p1{};
    p1.time_cost = 1; p1.memory_cost_kib = 8 * 1024; p1.parallelism = 1;
    PasswordHasher::Params p3{};
    p3.time_cost = 3; p3.memory_cost_kib = 8 * 1024; p3.parallelism = 1;

    PasswordHasher h1{p1};
    PasswordHasher h3{p3};

    std::set<std::string> s1, s3;
    for (int i = 0; i < 16; ++i) {
        s1.insert(h1.hash("x"));
        s3.insert(h3.hash("x"));
    }
    // 两个集合应当完全不相交（盐随机 + params 不同 → hash 必然不同）
    for (const auto& e : s1) {
        EXPECT_EQ(s3.count(e), 0u)
            << "hash from t=1 collided with hash from t=3 (probabilistically impossible)";
    }
}

TEST(PasswordHasherParamsSemantics, HigherMemoryCostYieldsDifferentHash) {
    PasswordHasher::Params pa{};
    pa.time_cost = 1; pa.memory_cost_kib = 4 * 1024;  pa.parallelism = 1;
    PasswordHasher::Params pb{};
    pb.time_cost = 1; pb.memory_cost_kib = 16 * 1024; pb.parallelism = 1;

    PasswordHasher ha{pa}, hb{pb};
    std::set<std::string> sa, sb;
    for (int i = 0; i < 16; ++i) {
        sa.insert(ha.hash("x"));
        sb.insert(hb.hash("x"));
    }
    for (const auto& e : sa) {
        EXPECT_EQ(sb.count(e), 0u);
    }
}

// ---------------------------------------------------------------------------
//  D. PHC 段级结构
// ---------------------------------------------------------------------------
namespace {
// 测试辅助：把 PHC 串切成 6 段（首尾两个空串）
std::vector<std::string> split_phc(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '$') { out.push_back(std::move(cur)); cur.clear(); }
        else          { cur.push_back(c); }
    }
    out.push_back(std::move(cur));
    return out;
}
}  // namespace

TEST(PasswordHasherPhcStructure, AllSegmentsArePopulated) {
    PasswordHasher h;
    const auto enc = h.hash("segment-check");
    const auto segs = split_phc(enc);
    ASSERT_EQ(segs.size(), 6u);
    EXPECT_EQ(segs[0], "");              // before first '$'
    EXPECT_EQ(segs[1], "argon2id");      // type
    EXPECT_EQ(segs[2], "v=19");          // version
    EXPECT_NE(segs[3].find("m="),    std::string::npos);  // params
    EXPECT_NE(segs[3].find(",t="),   std::string::npos);
    EXPECT_NE(segs[3].find(",p="),   std::string::npos);
    EXPECT_GT(segs[4].size(), 10u) << "salt segment too short";   // base64 of 16B
    EXPECT_GT(segs[5].size(), 30u) << "hash segment too short";   // base64 of 32B
}

TEST(PasswordHasherPhcStructure, SaltSegmentIsValidBase64) {
    // 16-byte salt → base64 长度 = ceil(16*8/6) = 22 chars
    // PHC Argon2 标准 base64 不带 padding（与 RFC 4648 §3.2 "URL-safe" 变体相同）
    PasswordHasher h;
    const auto enc = h.hash("salt-shape");
    const auto segs = split_phc(enc);
    ASSERT_EQ(segs.size(), 6u);
    EXPECT_EQ(segs[4].size(), 22u)
        << "expected 16-byte salt → 22-char base64 (no padding)";
    // base64 字符集：A-Z a-z 0-9 + / (无 '=' padding)
    for (char c : segs[4]) {
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '+' || c == '/';
        EXPECT_TRUE(ok) << "non-base64 char in salt: 0x" << std::hex
                        << static_cast<int>(static_cast<unsigned char>(c));
    }
}

TEST(PasswordHasherPhcStructure, HashSegmentIsValidBase64) {
    // 32-byte hash → base64 长度 = ceil(32*8/6) = 43 chars（无 padding）
    PasswordHasher h;
    const auto enc = h.hash("hash-shape");
    const auto segs = split_phc(enc);
    ASSERT_EQ(segs.size(), 6u);
    EXPECT_EQ(segs[5].size(), 43u)
        << "expected 32-byte hash → 43-char base64 (no padding)";
    for (char c : segs[5]) {
        const bool ok = (c >= 'A' && c <= 'Z') ||
                        (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') ||
                        c == '+' || c == '/';
        EXPECT_TRUE(ok) << "non-base64 char in hash: 0x" << std::hex
                        << static_cast<int>(static_cast<unsigned char>(c));
    }
}

TEST(PasswordHasherPhcStructure, DifferentParamsProduceDifferentParamsSegment) {
    PasswordHasher::Params p1{};
    p1.time_cost = 1; p1.memory_cost_kib = 8 * 1024;  p1.parallelism = 1;
    PasswordHasher::Params p2{};
    p2.time_cost = 2; p2.memory_cost_kib = 16 * 1024; p2.parallelism = 2;

    PasswordHasher h1{p1}, h2{p2};
    const auto e1 = split_phc(h1.hash("x"));
    const auto e2 = split_phc(h2.hash("x"));
    EXPECT_NE(e1[3], e2[3]) << "params segment should reflect hasher params";
}

// ---------------------------------------------------------------------------
//  E. Unicode 边界
// ---------------------------------------------------------------------------
TEST(PasswordHasherUnicode, EmojiPasswordRoundTrip) {
    PasswordHasher h;
    const std::string pw = "🔒secure🔑password🎉";  // 多字节 UTF-8
    const auto enc = h.hash(pw);
    EXPECT_TRUE(h.verify(pw, enc));
    EXPECT_FALSE(h.verify("🔒secure🔑passwor",   enc));  // 少一个 emoji
    EXPECT_FALSE(h.verify("🔒secure🔑password🎊", enc));  // 错一个 emoji
}

TEST(PasswordHasherUnicode, CombiningCharactersTreatedAsBytes) {
    // 同一个逻辑字符但用预组合 vs 分解形式：UTF-8 字节序列不同，
    // Argon2id 按字节处理，所以 verify 必须字节级一致。
    PasswordHasher h;
    const std::string precomposed  = "café";           // é = U+00E9 (2 bytes)
    const std::string decomposed  = "cafe\xCC\x81";   // e + combining acute (3 bytes total)
    const auto enc = h.hash(precomposed);
    EXPECT_TRUE (h.verify(precomposed, enc));
    EXPECT_FALSE(h.verify(decomposed, enc))
        << "Argon2id should distinguish precomposed vs decomposed Unicode";
}

TEST(PasswordHasherUnicode, NullByteInPasswordHandledSafely) {
    // C 字符串里 '\0' 是终止符，但 std::string 用 size 区分长度；
    // 验证 libargon2 不会被内部 C-string 化截断。
    // 注意：必须显式指定长度 9，否则字面量 "pass\0word" 会被截断为 "pass"。
    const std::string pw("pass\0word", 9);
    ASSERT_EQ(pw.size(), 9u);
    PasswordHasher h;
    const auto enc = h.hash(pw);
    EXPECT_TRUE(h.verify(pw, enc));
    EXPECT_FALSE(h.verify("password", enc))   // 不带 NUL 的版本不同
        << "Argon2id must distinguish embedded NUL from terminator";
}

TEST(PasswordHasherUnicode, AllBytesHighBitSetRoundTrip) {
    // 0x80-0xFF 全高位置字节（Latin-1 字符 / UTF-8 续字节）
    PasswordHasher h;
    std::string pw;
    for (int i = 0; i < 64; ++i) {
        pw.push_back(static_cast<char>(0x80 + (i % 0x80)));
    }
    const auto enc = h.hash(pw);
    EXPECT_TRUE(h.verify(pw, enc));
}

// ---------------------------------------------------------------------------
//  F. 抗指纹：encoded 串不能泄漏明文 / 盐字节
// ---------------------------------------------------------------------------
TEST(PasswordHasherAntiLeak, EncodedDoesNotContainPlaintext) {
    // 反复 hash 同一段强可识别密码，确认 encoded 中找不到明文子串
    PasswordHasher h;
    const std::string pw = "Sup3rUn1qu3P@ssw0rd!";

    for (int i = 0; i < 10; ++i) {
        const auto enc = h.hash(pw);
        EXPECT_EQ(enc.find(pw), std::string::npos)
            << "plaintext leaked into encoded hash (iter " << i << ")";
    }
}

TEST(PasswordHasherAntiLeak, DifferentPasswordsProduceUnrelatedHashes) {
    // 两个仅一字之差的密码 hash 值应截然不同（雪崩效应）
    PasswordHasher h;
    const auto e1 = h.hash("password-A");
    const auto e2 = h.hash("password-B");
    EXPECT_NE(e1, e2);

    // 共享前缀计数：PHC 头部都是 "$argon2id$v=19$m=65536,t=3,p=4$"，
    // 这是协议常量，不算"相似"——只看后段
    std::size_t shared = 0;
    for (std::size_t i = 0; i < std::min(e1.size(), e2.size()); ++i) {
        if (e1[i] == e2[i]) ++shared;
        else break;
    }
    EXPECT_LT(shared, e1.size() / 2)
        << "hashes share too many leading chars — avalanche weak";
}

// ---------------------------------------------------------------------------
//  G. 参数边界
// ---------------------------------------------------------------------------
TEST(PasswordHasherParamsBoundary, AcceptsMinimumLegalValues) {
    PasswordHasher::Params p{};
    p.time_cost       = 1;
    p.memory_cost_kib = 8;       // libargon2 ARGON2_MIN_MEMORY = 8 KiB
    p.parallelism     = 1;
    p.salt_len        = 8;       // libargon2 ARGON2_MIN_SALT_LENGTH
    p.hash_len        = 4;       // libargon2 ARGON2_MIN_OUTLEN
    EXPECT_NO_THROW({
        PasswordHasher h{p};
        const auto enc = h.hash("x");
        EXPECT_TRUE(h.verify("x", enc));
    });
}

TEST(PasswordHasherParamsBoundary, RejectsJustBelowMinimum) {
    {
        PasswordHasher::Params p{};
        p.time_cost       = 1;
        p.memory_cost_kib = 7;       // ARGON2_MIN_MEMORY - 1
        EXPECT_THROW(PasswordHasher{p}, HashError);
    }
    {
        PasswordHasher::Params p{};
        p.salt_len = 7;             // ARGON2_MIN_SALT_LENGTH - 1
        EXPECT_THROW(PasswordHasher{p}, HashError);
    }
    {
        PasswordHasher::Params p{};
        p.hash_len = 3;             // ARGON2_MIN_OUTLEN - 1
        EXPECT_THROW(PasswordHasher{p}, HashError);
    }
    {
        PasswordHasher::Params p{};
        p.parallelism = 0;          // 至少 1 lane
        EXPECT_THROW(PasswordHasher{p}, HashError);
    }
}

TEST(PasswordHasherParamsBoundary, DefaultConstructorUsesOwaspBaseline) {
    PasswordHasher h;
    const auto p = h.params();
    EXPECT_EQ(p.time_cost,       3u);
    EXPECT_EQ(p.memory_cost_kib, 64u * 1024u);
    EXPECT_EQ(p.parallelism,     4u);
    EXPECT_EQ(p.salt_len,        16u);
    EXPECT_EQ(p.hash_len,        32u);
}

// ---------------------------------------------------------------------------
//  H. is_encoded_hash 段数严格匹配
// ---------------------------------------------------------------------------
TEST(PasswordHasherIsEncodedHash, RequiresAtLeastFiveDollarSegments) {
    // is_encoded_hash 是"启发式判断看起来像 Argon2 PHC"，用于决定要不要把
    // 一段密文喂给 argon2id_verify。它的设计取向是**容错**（宁可误报也不要
    // 漏报——真正严格的格式校验在 libargon2 内部完成）。
    // 因此：少于 5 段 → false；至少 5 段 → true。

    // 4 段（缺 hash）→ false
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=1,t=1,p=1$c29tZXNhbHQ"));

    // 5 段 + 末尾 hash 为空 → 仍视为"像"（heuristic 不会校验子段非空）
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=1,t=1,p=1$c29tZXNhbHQ$"));

    // 6 段 → heuristic 也视为"像"（后续 verify 会拒掉）
    EXPECT_TRUE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=1,t=1,p=1$c29tZXNhbHQ$xxx$extra"));
}

TEST(PasswordHasherIsEncodedHash, RequiresMinimumLength) {
    // < 32 字节一律 false（启发式拒绝太短的"形似"串）
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(""));
    EXPECT_FALSE(PasswordHasher::is_encoded_hash("$argon2id$v=19$m=1$"));  // 17 字节
    EXPECT_FALSE(PasswordHasher::is_encoded_hash(
        "$argon2id$v=19$m=1,t=1,p=1$short"));  // < 32
}

// ---------------------------------------------------------------------------
//  I. Determinism: verify() 是纯函数
// ---------------------------------------------------------------------------
TEST(PasswordHasherVerifyDeterminism, MultipleCallsReturnSameResult) {
    PasswordHasher h;
    const auto enc = h.hash("deterministic-pw");

    for (int i = 0; i < 20; ++i) {
        EXPECT_TRUE(h.verify("deterministic-pw", enc));
        EXPECT_FALSE(h.verify("wrong-pw",         enc));
    }
}

TEST(PasswordHasherVerifyDeterminism, VerifyDoesNotMutateEncoded) {
    // verify() 应当是 const + noexcept + 无副作用
    PasswordHasher h;
    const auto enc = h.hash("no-mutation");
    const std::string enc_snapshot = enc;

    for (int i = 0; i < 5; ++i) {
        (void)h.verify("correct",     enc);
        (void)h.verify("wrong",       enc);
        (void)h.verify("",            enc);
        (void)h.verify(std::string(500, 'x'), enc);
    }
    EXPECT_EQ(enc, enc_snapshot)
        << "verify() must not mutate its encoded argument";
}

}  // namespace