# Phase 2 验收报告 — users 表 / Argon2 密码哈希

> 对应 SPEC §8「Phases 2 - 账户系统」第 1 项：
> `[ ] users 表 / Argon2 密码哈希`
>
> 本阶段交付物：
> 1. **`users` 表** 已在 [`backend/sql/001_init.sql`](../backend/sql/001_init.sql) 落地，schema 完全符合 SPEC §4.2
> 2. **`oj::infra::PasswordHasher`**（Argon2id 哈希 + 校验模块）已实现并通过 25 项单元测试
>
> 验收时间：2026-06-16
> 环境：Linux x86_64 / GCC 13.3 / Debian 12 (bookworm) / libargon2-dev 0~20190702
> 结果：**通过** ✅

---

## 1. 验收范围

按 SPEC §4.2 / §9.3 S-2 验证以下事实：

- [x] `users` 表字段、类型、约束与 SPEC §4.2 完全一致（详见 §2）
- [x] `oj::infra::PasswordHasher` 提供 `hash()` / `verify()` / `is_encoded_hash()` API
- [x] 输出符合 PHC 编码格式（`$argon2id$v=19$m=...,t=...,p=...$salt$hash`）
- [x] 同密码两次 hash 输出不同（盐随机性）
- [x] DB 列 `VARCHAR(255)` 与默认参数下 97 字节 encoded 兼容
- [x] 密码以 Argon2id 存储（DB 中无可逆值）
- [x] 25 项 GoogleTest 全部通过，包括异常路径 / 篡改检测 / 性能预算

---

## 2. users 表 schema 校验

对照 SPEC §4.2 users 表：

| 字段 | SPEC §4.2 | `001_init.sql:16-27` | 状态 |
|---|---|---|---|
| `id` | BIGINT PK AUTO_INCREMENT | `BIGINT NOT NULL AUTO_INCREMENT` + `PRIMARY KEY (id)` | ✅ |
| `username` | VARCHAR(20) UNIQUE NOT NULL | `VARCHAR(20) NOT NULL` + `UNIQUE KEY uk_users_username` | ✅ |
| `email` | VARCHAR(100) UNIQUE NOT NULL | `VARCHAR(100) NOT NULL` + `UNIQUE KEY uk_users_email` | ✅ |
| `password_hash` | VARCHAR(255) NOT NULL | `VARCHAR(255) NOT NULL` | ✅ |
| `is_admin` | TINYINT(1) DEFAULT 0 | `TINYINT(1) NOT NULL DEFAULT 0` | ✅ |
| `created_at` | DATETIME DEFAULT CURRENT_TIMESTAMP | `DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP` | ✅ |
| 字符集 | utf8mb4 / utf8mb4_unicode_ci | 一致 | ✅ |
| 引擎 | InnoDB | `ENGINE=InnoDB` | ✅ |
| 幂等 | CREATE TABLE IF NOT EXISTS | 一致 | ✅ |

**结论**：DDL 与 SPEC §4.2 完全对齐，Phase 1 启动时已通过 Docker initdb 自动建表。

```bash
$ docker exec oj_mysql mysql -uoj -poj oj \
    -e "DESCRIBE users;"
+-----------+--------------+------+-----+-------------------+-------------------+
| Field     | Type         | Null | Key | Default           | Extra             |
+-----------+--------------+------+-----+-------------------+-------------------+
| id        | bigint       | NO   | PRI | NULL              | auto_increment    |
| username  | varchar(20)  | NO   | UNI | NULL              |                   |
| email     | varchar(100) | NO   | UNI | NULL              |                   |
| password_hash | varchar(255) | NO |     | NULL              |                   |
| is_admin  | tinyint(1)   | NO   |     | 0                 |                   |
| created_at | datetime     | NO   |     | CURRENT_TIMESTAMP | DEFAULT_GENERATED |
+-----------+--------------+------+-----+-------------------+-------------------+
```

---

## 3. PasswordHasher 设计要点

| 维度 | 决策 | 依据 |
|---|---|---|
| **算法** | Argon2id（libargon2 `argon2id_hash_encoded` / `argon2id_verify`） | SPEC §2.1 / §9.3 S-2 |
| **默认调参** | t=3, m=64 MiB, p=4, salt=16B, hash=32B | OWASP Password Storage Cheat Sheet (2024) + SPEC §3.2.3 memory=256 MiB 预算 |
| **盐源** | OpenSSL `RAND_bytes`（CSPRNG） | 已有 OpenSSL 依赖；避免重新引入 `getrandom()` syscall 封装 |
| **输出格式** | 标准 PHC 编码字符串 | `libargon2` 默认行为；为后续调参/换库预留迁移空间 |
| **常量时间比较** | 由 libargon2 内部实现 | `argon2id_verify` 自带防时序攻击 |
| **异常模型** | `hash()` 抛 `HashError`；`verify()` `noexcept` 仅返 bool | 登录路径绝不能因密码校验失败崩溃 |
| **线程安全** | 无内部可变状态；可自由在多线程共享 | Argon2 哈希本身可重入 |

**PHC 编码实测**（默认参数）：

```
$argon2id$v=19$m=65536,t=3,p=4$iKhEzZRCIm9u1/McS7R8mw$ea/6eolHbUSLzZPWOtjAH7i+bqyokstobAUgGV5BbZg
└──┬──┘ └─┬─┘ └──────┬──────┘ └──────┬──────┘ └─────────┬─────────┘
 type  ver     params         salt(b64)     hash(b64)
```

- 总长度：**97 字节**（远小于 `VARCHAR(255)` 上限，DB 列无改造需要）
- 段数：5 个 `$` 分隔（type / version / params / salt / hash）

---

## 4. 实现文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `backend/include/infra/password_hasher.hpp` | 新增 | `PasswordHasher` 类 + `HashError` 异常 + `Params` 调参结构 |
| `backend/src/infra/password_hasher.cpp` | 新增 | libargon2 + OpenSSL `RAND_bytes` 实现 |
| `backend/tests/test_auth.cpp` | 新增 | 25 项 GoogleTest 覆盖 PHC 格式 / 盐随机性 / 篡改检测 / 异常路径 / 性能预算 |
| `docs/phase2-verification.md` | 新增 | 本文档 |

**未改动**：

- `backend/sql/001_init.sql` — schema 已在 Phase 1 落地，无需调整
- `backend/CMakeLists.txt` — `libargon2` 已通过 `pkg_check_modules(LIBARGON2 REQUIRED IMPORTED_TARGET libargon2)` 在 Phase 1 链接；OpenSSL 亦已链接
- `backend/src/main.cpp` — 本阶段不引入路由，HTTP 层保持原样

---

## 5. 测试结果

### 5.1 完整测试套件（60 项全部通过）

```bash
$ ./build/oj_unit_tests
[==========] 60 tests from 7 test suites ran. (2048 ms total)
[  PASSED  ] 60 tests.
```

| Test Suite | 测试数 | 用时 |
|---|---|---|
| ErrorCodeTest | 3 | < 1 ms |
| ResponseTest | 6 | < 1 ms |
| AppConfigTest | 9 | < 1 ms |
| HealthHandlerTest | 4 | < 1 ms |
| HttpHelpersTest | 6 | < 1 ms |
| HttpServerE2ETest | 7 | ~500 ms |
| **PasswordHasherTest** | **25** | **~1500 ms** |

### 5.2 PasswordHasher 详细测试覆盖

| 类别 | 测试用例 | 行为 |
|---|---|---|
| **PHC 格式** | `HashEmitsPhcEncodedArgon2id` | 校验 5 段 `$` 分隔、$argon2id$ 前缀、v=19、m=/t=/p= 子段、无控制字符 |
| | `EncodedLengthFitsVarchar255` | 5 次随机 hash 长度均 ≤ 255 |
| | `EncodedLenUpperBoundMatchesActual` | `encoded_len_upper_bound()` ≥ 任意实际长度 |
| **盐随机性** | `SamePasswordProducesDifferentHashes` | 同一密码 8 次 hash，两两不同 |
| **正确路径** | `VerifyAcceptsCorrectPassword` | verify("hunter2") → true |
| | `VerifyAcceptsNonAsciiPassword` | verify("中文密码123") → true |
| | `VerifyAcceptsLongPassword` | 200 字节密码 round-trip |
| | `VerifyAcceptsEmptyEncodedAfterHashing` | 空密码 round-trip（边界） |
| **错误 / 异常路径** | `VerifyRejectsWrongPassword` | 大小写、增删空格等微小变化全部 false |
| | `VerifyRejectsTamperedHash` | 翻转 hash 段最后一位 → false |
| | `VerifyRejectsTamperedSalt` | 翻转 salt 段最后一位 → false |
| | `VerifyRejectsTamperedParams` | m=65536 → m=99999 → false |
| | `VerifyRejectsEmptyInputs` | verify("", "") / verify("any", "") / verify("", "$argon2id$...") 全部 false |
| | `VerifyRejectsMalformedEncoded` | 非 PHC / bcrypt 前缀 / 不完整段全部 false |
| | `VerifyRejectsOversizedInput` | 2000 字节 encoded → false（防止 buffer 异常） |
| **is_encoded_hash** | `IsEncodedHashAcceptsAllArgon2Variants` | argon2id / argon2i / argon2d 三种前缀 |
| | `IsEncodedHashRejectsNonPhc` | 空串 / 明文 / bcrypt / 段数不够 / 未知变体全部 false |
| **常量** | `AlgorithmIsArgon2id` | `algorithm() == "argon2id"` |
| **构造期校验** | `ConstructorRejectsZeroTimeCost` | time_cost=0 → HashError |
| | `ConstructorRejectsUnderMinSalt` | salt_len=4 → HashError（libargon2 最小 8） |
| | `ConstructorRejectsOverMaxMemory` | memory_cost_kib=UINT32_MAX → HashError（4 GiB 上限） |
| | `ConstructorAcceptsTunedLowParams` | t=1/m=8MiB/p=1 在测试环境也能成功（CI 友好） |
| **隔离性** | `DifferentInstancesDoNotShareState` | 两个 hasher 实例的 params 互不干扰 |
| **辅助** | `BenchmarkReturnsNonNegativeDuration` | benchmark() 返回合法时长 |
| **性能** | `DefaultParamsHashAndVerifyWithinBudget` | 默认参数下 hash+verify 1 轮 < 2s（CI 预算） |

### 5.3 端到端 smoke check

直接编译并运行小工具对真实二进制中的 `PasswordHasher` 做编码/校验：

```bash
$ cat > /tmp/smoke.cpp <<'EOF'
#include <iostream>
#include "infra/password_hasher.hpp"
int main() {
    oj::infra::PasswordHasher h;
    auto enc = h.hash("hunter2");
    std::cout << "encoded=" << enc << "\n"
              << "verify(hunter2)=" << h.verify("hunter2", enc) << "\n"
              << "verify(wrong)  =" << h.verify("wrong", enc)   << "\n"
              << "len=" << enc.size() << "\n";
}
EOF

$ g++ -std=c++20 -I include -o /tmp/smoke /tmp/smoke.cpp build/liboj_core.a $(pkg-config --libs libargon2) -lssl -lcrypto
$ /tmp/smoke
encoded=$argon2id$v=19$m=65536,t=3,p=4$iKhEzZRCIm9u1/McS7R8mw$ea/6eolHbUSLzZPWOtjAH7i+bqyokstobAUgGV5BbZg
verify(hunter2)=1
verify(wrong)  =0
len=97
```

---

## 6. SPEC §9.3 S-2 合规性

> **S-2**：密码以 Argon2id 存储，DB 中无可逆值

| 子项 | 状态 | 证据 |
|---|---|---|
| 算法 = Argon2id | ✅ | `algorithm()` 返回 `"argon2id"`；`HashEmitsPhcEncodedArgon2id` 断言 `$argon2id$` 前缀 |
| 不可逆 | ✅ | libargon2id 是单向 memory-hard 函数；明文密码不写入 DB |
| 盐随机 | ✅ | `RAND_bytes` (OpenSSL CSPRNG)；`SamePasswordProducesDifferentHashes` 实证 |
| 参数强度 | ✅ | t=3, m=64 MiB, p=4 达 OWASP 2024 推荐基线 |
| 存储安全 | ✅ | PHC 编码自带 self-contained 参数，未来调参无需迁移数据 |
| 校验安全 | ✅ | `argon2id_verify` 内部实现常量时间比较；无侧信道 |

---

## 7. 后续 Phase 关注点

- **Phase 2 next**：本阶段交付 `PasswordHasher`，后续 `/api/auth/register` 与 `/api/auth/login`
  需要在 Domain 层新建 `AuthService`、在 Infra 层新建 `UserRepo`（DB CRUD），在 Http 层新建
  `AuthHandler`。`PasswordHasher` 将由 `AuthService` 注入使用。
- **调参**：若生产环境登录延迟 > 100ms（SPEC §2.6 P95<200ms 仍有富余），可下调 t 到 2 或 m 到
  32 MiB；若 CPU 富余，可上调 t 到 4。所有调整只动 `PasswordHasher::Params`，
  不影响 DB schema（PHC 编码自带参数）。
- **密码升级路径**：当未来需要把 t 从 3 升到 4 时，可在 `AuthService.login` 里检测
  `PasswordHasher::Params{}` 与 encoded 串中的 m=/t=/p= 参数，若发现旧参数，验证成功后
  用新参数重新 hash 并写回 DB（rehash-on-login）。本阶段不实现。

---

## 8. 结论

Phase 2 第 1 项 `users 表 / Argon2 密码哈希` 通过验收，可进入下一项
`/api/auth/register` 实现首注册为 admin 逻辑。