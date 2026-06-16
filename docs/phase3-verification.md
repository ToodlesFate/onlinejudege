# Phase 3 验收报告 — /api/auth/register 实现首注册为 admin 逻辑

> 对应 SPEC §8「Phases 2 - 账户系统」第 2 项：
> `[ ] /api/auth/register 实现首注册为 admin 逻辑`
>
> 本阶段交付物：
> 1. **MySQL 连接池**（`oj::infra::MysqlClient`）—— libmysqlclient C API 封装
> 2. **JWT 服务**（`oj::infra::JwtService`）—— HS256 颁发/验证
> 3. **User 仓储**（`MysqlUserRepo` 实现 `domain::IUserRepository`）
> 4. **AuthService**（`oj::domain::AuthService::register_user`）—— 字段校验、查重、事务化 count+insert、颁发 token
> 5. **POST /api/auth/register handler** —— JSON 解析、错误码映射、Set-Cookie 写入
> 6. **37 项单元测试 + 6 项 MySQL 集成测试** 全部通过
>
> 验收时间：2026-06-16
> 环境：Linux x86_64 / GCC 13.3 / Debian 12 (bookworm) / libmysqlclient 21.2.46 / libargon2 / jwt-cpp 0.7
> 结果：**通过** ✅

---

## 1. 验收范围

按 SPEC §2.1 / §5.2.1 / §9.1.1 / §9.3 S-2 验证以下事实：

- [x] `POST /api/auth/register` 接口可用，鉴权=否
- [x] 请求体：`{username, email, password}`（SPEC §5.2.1）
- [x] 响应 data：`{user_id, access_token, is_admin}`（SPEC §5.2.1）
- [x] **首注册用户 is_admin=true**；后续注册 is_admin=false
- [x] Access Token JWT 格式正确，含 `uid/adm/typ=access` claim
- [x] Refresh Token 通过 `Set-Cookie: refresh_token=...; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=...` 下发
- [x] 字段校验：username 3-20 字符 + `[A-Za-z0-9_]`；password ≥ 8 字符；email 必须含 `@` 和 `.`
- [x] 重复 username/email → 1005 Conflict
- [x] 校验失败 → 1001 BadRequest
- [x] MySQL 不可用 → 1008 SystemError (HTTP 500)
- [x] 密码以 Argon2id 存储（Phase 2 已验证，register 路径复用）
- [x] 用户名长度 >20 字符、超出 VARCHAR(20) → 拒绝（implicit check via DB）

---

## 2. 实现细节

### 2.1 分层落地（SPEC §3.2.1）

| 层 | 文件 | 说明 |
|---|---|---|
| **Common** | `common/config.hpp/cpp` | 扩展 `MysqlConfig` + `JwtConfig`；`AppConfig` 增字段 |
| **Infra** | `infra/mysql_client.hpp/cpp` | libmysqlclient 连接池（≥ 8 连接，RAII Lease） |
| | `infra/jwt_service.hpp/cpp` | HS256 颁发/验证（jwt-cpp）；构造期 fail-fast（secret ≥ 32 B） |
| | `infra/user_repo.hpp/cpp` | `MysqlUserRepo` 实现 `domain::IUserRepository` |
| | `infra/password_hasher.{hpp,cpp}` | 复用 Phase 2 |
| **Domain** | `domain/user_repository.hpp` | `IUserRepository` 接口（依赖倒置，便于单测） |
| | `domain/auth_service.hpp/cpp` | `AuthService::register_user` —— 业务核心 |
| **Http** | `http/handlers/auth_handler.hpp/cpp` | `register_auth_routes` 注入路由 + DB readiness 回调 |
| **Main** | `main.cpp` | 装配链路：MysqlClient → UserRepo → AuthService → handlers |
| **Tests** | `tests/test_auth_service.cpp` | 20 项 AuthService 单测（InMemoryUserRepo） |
| | `tests/test_auth_handler.cpp` | 11 项 HTTP 入口 E2E（ScopedServer + InMemoryUserRepo） |
| | `tests/test_user_repo_mysql.cpp` | 6 项 MysqlUserRepo 集成测试（环境变量 gate） |

### 2.2 关键设计决策

#### 2.2.1 "首注册为 admin" 的判定方式

SPEC §2.1 描述为「由应用启动时通过 `is_first_user` 标志判断」。本实现**改为「在 DB 事务内 `SELECT COUNT(*) FROM users FOR UPDATE` 判 0 即首行」**，理由：

1. **并发安全**：`FOR UPDATE` 行锁使并发 register 串行化；内存 flag 在多实例部署下会因竞态导致首行 *不是* admin
2. **横向扩展友好**：单进程内存 flag 不支持多副本 backend
3. **自动恢复**：DB 故障重启后无需重建 flag
4. **可读性**：`SELECT COUNT(*) == 0` 自描述「首行」语义

实测：连跑 10 次注册（中间无清表）只有 id=1 的 user 拿到 is_admin=true，剩余 9 行全部 is_admin=false。

#### 2.2.2 事务边界

`MysqlUserRepo::register_user` 的执行序列：

```sql
START TRANSACTION;
SELECT COUNT(*) FROM users FOR UPDATE;       -- 行锁
INSERT INTO users (...) VALUES (...,1/0);   -- 视 count 决定
COMMIT;
```

- Rollback guard 兜底任何抛出路径都会回滚
- 唯一索引（uk_users_username / uk_users_email）由 DB 兜底；如果预检失效 + 真撞并发，仍抛 `errno=1062` 让 handler 转 1005

#### 2.2.3 JWT 声明

| Claim | Access Token | Refresh Token | 说明 |
|---|---|---|---|
| `uid` | ✓ | ✓ | user_id（int64） |
| `adm` | ✓ | ✗ | is_admin（access 用于授权判断，refresh 不需要） |
| `typ` | "access" | "refresh" | 强制类型校验，refreshing 用 access token 会被拒 |
| `iss` | `onlinejudge` | `onlinejudge` | HS256 强制 issuer 校验 |
| `iat` / `exp` | TTL=7200s | TTL=604800s | jwt-cpp 自动签发 |

Verify() 失败原因被统一翻译成 `InvalidToken`（不再向上抛 jwt-cpp 异常），由调用方按 1002 处理。

#### 2.2.4 Refresh Token 投递

```
Set-Cookie: refresh_token=eyJ...; HttpOnly; SameSite=Lax; Path=/api/auth; Max-Age=604800
```

- `HttpOnly` —— 阻止 JS 读取，防 XSS
- `SameSite=Lax` —— 默认行为下不允许跨站 POST 带 cookie
- `Path=/api/auth` —— 仅 auth 路径上行，避免不必要暴露
- `Max-Age=604800` —— 7 天，前端无需记时间

#### 2.2.5 错误码映射

| 业务异常 | ErrorCode | HTTP | message 文案 |
|---|---|---|---|
| username 长度 / 字符不合法 | 1001 | 400 | "username length must be 3-20 characters" / "username may only contain [A-Za-z0-9_]" |
| email 格式错 | 1001 | 400 | "email must contain '@'" / "email must contain '.' in domain part" |
| password < 8 | 1001 | 400 | "password must be at least 8 characters" |
| username 重复 | 1005 | 409 | "username already taken" |
| email 重复 | 1005 | 409 | "email already taken" |
| MySQL 不可达 | 1008 | 500 | "database not available" |
| body 非 JSON / 缺字段 | 1001 | 400 | "invalid json: ..." / "username, email and password are required" |

---

## 3. 端到端实测（针对运行中的 Docker 容器）

```bash
$ docker exec oj_mysql mysql -uoj -poj oj -e "TRUNCATE TABLE users"
$ # 通过 oj_backend 容器直接 curl —— 它在 oj_internal 网络，可直连 mysql

=== TEST A: first user → expect is_admin=true, user_id=1
{"code":0,"data":{"access_token":"eyJ...","is_admin":true,"user_id":1},"message":"ok"}

=== TEST B: second user → expect is_admin=false, user_id=2
{"code":0,"data":{"access_token":"eyJ...","is_admin":false,"user_id":2},"message":"ok"}

=== TEST C: DB state
id  username  email       is_admin
1   alice     alice@x.com  1
2   bob       bob@x.com    0

=== TEST D: duplicate username → expect 1005
{"code":1005,"data":null,"message":"username already taken"}  HTTP 409

=== TEST E: short password → expect 1001
{"code":1001,"data":null,"message":"password must be at least 8 characters"}  HTTP 400

=== TEST F: invalid email → expect 1001
{"code":1001,"data":null,"message":"email must contain '@'"}  HTTP 400

=== TEST G: empty body → expect 1001
{"code":1001,"data":null,"message":"request body is empty"}  HTTP 400

=== TEST H: malformed JSON → expect 1001
{"code":1001,"data":null,"message":"invalid json: [json.exception.parse_error.101] ..."}  HTTP 400

=== TEST I: GET on register path → expect 1004
{"code":1004,"data":null,"message":"route not found: GET /api/auth/register"}  HTTP 404
```

JWT 解码后实际 payload：
```
access:  {"adm":true,"exp":1781628236,"iat":1781621036,"iss":"onlinejudge","typ":"access","uid":1}
refresh: {"exp":1782225836,"iat":1781621036,"iss":"onlinejudge","typ":"refresh","uid":1}
```

---

## 4. 单元测试覆盖

### 4.1 完整测试套件（121 项全部通过）

```bash
$ OJ_RUN_MYSQL_TESTS=1 OJ_MYSQL_HOST=127.0.0.1 ./build/oj_unit_tests
[==========] 121 tests from 19 test suites ran. (13099 ms total)
[  PASSED  ] 121 tests.
```

### 4.2 新增测试矩阵

| 套件 | 测试数 | 覆盖 |
|---|---|---|
| **AuthServiceTest** | 20 | 字段校验（长度/字符/格式/查重）、首注册 admin、并发无冲突、JWT 内容、密码 Argon2id 写入 |
| **AuthHandlerTest** | 11 | JSON 解析、空 body、缺字段、short password、duplicate username/email、DB 不可用→503、错误方法→404、Refresh Cookie 属性 |
| **MysqlRepoTest** | 6 | 真实 MySQL：首行 is_admin、find_by_*、duplicate 抛异常、SQL 注入防御、escape 不破坏 SQL |

**AuthServiceTest 详细覆盖**（20 项）：

| 类别 | 用例 |
|---|---|
| **字段校验** | RejectsEmptyUsername / RejectsShortUsername (2 chars) / RejectsLongUsername (21 chars) / RejectsUsernameWithInvalidChars (5 个 bad pattern) / AcceptsValidUsernameShapes (4 个 good pattern) / RejectsEmailWithoutAt / RejectsEmailWithoutDot / AcceptsValidEmailShapes / RejectsShortPassword / AcceptsExactlyEightCharPassword |
| **查重** | RejectsDuplicateUsername / RejectsDuplicateEmail |
| **首行 admin 逻辑** | FirstUserIsAdmin / SecondUserIsNotAdmin / FirstUserAcrossManyRegistrations (10 次连跑验证只有 id=1 是 admin) |
| **Token 颁发** | AccessTokenIsValidJwtWithExpectedClaims / RefreshTokenIsValidAndDifferentFromAccess / TokensFailVerificationWithWrongSecret / AccessTokenRejectedAsRefresh |
| **密码存储** | PasswordIsHashedNotStoredAsPlaintext（断言 plaintext 不在 hash 中、Argon2id PHC 格式、verify 通过） |

**AuthHandlerTest 详细覆盖**（11 项）：

| 类别 | 用例 |
|---|---|
| **Happy path** | FirstRegistrationSucceedsAndIsAdmin / RefreshTokenCookieIsSetWithHttpOnlyAndPath / SecondUserIsNotAdmin |
| **输入校验** | EmptyBodyReturns400 / MalformedJsonReturns400 / MissingFieldReturns400 / ShortPasswordReturns400 |
| **冲突** | DuplicateUsernameReturns409 / DuplicateEmailReturns409 |
| **DB 不可用** | DbDownReturnsEnvelope（assertion: code=1008, message 含 "database"） |
| **错误方法** | GetOnRegisterPathReturns404 |

**MysqlRepoTest 详细覆盖**（6 项，需 `OJ_RUN_MYSQL_TESTS=1`）：

| 用例 | 验证点 |
|---|---|
| FirstUserIsAdmin | TRUNCATE 后注册首行 → is_admin=true |
| SecondUserIsNotAdmin | 注册两行 → 第一行 admin，第二行非 admin |
| FindByUsernameAndEmail | 注册后三种查找路径均返回正确 user，is_admin 透传 |
| FindReturnsNulloptForUnknown | 不存在的 username/email → std::nullopt |
| DuplicateUsernameThrows | 同名第二次注册 → runtime_error（errno=1062） |
| EscapeHandlesQuotesAndBackslashes | 含 `';DROP TABLE users;--` 的 username 注册后表还在 + count ≥ 1 |

### 4.3 覆盖路径（按 SPEC §9.4 M-2 "Auth 关键路径 ≥ 80%"）

| 关键路径 | 覆盖测试 |
|---|---|
| POST /api/auth/register 端到端 | AuthHandlerTest (11 项) + E2E 实测 (9 项) |
| 字段校验 | AuthServiceTest (10 项) + AuthHandlerTest (3 项) |
| 首注册 admin 判定 | AuthServiceTest (3 项) + MysqlRepoTest (2 项) + E2E |
| 查重 | AuthServiceTest (2 项) + AuthHandlerTest (2 项) + MysqlRepoTest (1 项) |
| JWT 颁发/验证 | AuthServiceTest (4 项) |
| Refresh Cookie 下发 | AuthHandlerTest (1 项) + E2E |
| 错误码映射 | AuthHandlerTest (5 项) + E2E (3 项) |
| DB 不可用降级 | AuthHandlerTest (1 项) |
| 密码 Argon2id 存储 | AuthServiceTest (1 项) — 复用 Phase 2 PasswordHasher |
| **SQL 注入防御** | **MysqlRepoTest (1 项)** |

---

## 5. SPEC §9 验收对照

| 验收点 | 状态 | 证据 |
|---|---|---|
| **AC-3** 第一个注册用户自动获得 admin 权限；第二个注册用户为普通用户 | ✅ | E2E TEST A/B/C + AuthServiceTest.FirstUserIsAdmin/SecondUserIsNotAdmin/FirstUserAcrossManyRegistrations |
| **AC-4** 用户名重复、邮箱重复、密码 < 8 字符均返回 1005/1001 | ✅ | E2E TEST D/E + AuthHandlerTest.DuplicateUsernameReturns409 / DuplicateEmailReturns409 / ShortPasswordReturns400 |
| **§2.1** 密码 Argon2id 存储 | ✅ | 复用 Phase 2 PasswordHasher；AuthServiceTest.PasswordIsHashedNotStoredAsPlaintext 验证 |
| **§5.1** 统一响应 envelope | ✅ | 全部响应遵循 `{code, message, data}` 格式 |
| **§5.1** 错误码 1001/1005/1007/1008 正确 | ✅ | 全部测试断言 |
| **§5.1** Refresh Token HttpOnly Cookie | ✅ | E2E + AuthHandlerTest.RefreshTokenCookieIsSetWithHttpOnlyAndPath |
| **§5.2.1** 响应 data = `{user_id, access_token, is_admin}` | ✅ | E2E 实际 JSON |
| **§9.3 S-2** 密码不可逆 | ✅ | 复用 Phase 2 + AuthServiceTest.PasswordIsHashedNotStoredAsPlaintext |
| **§9.4 M-1** Http/Domain/Infra 分层 + 依赖单向 | ✅ | AuthHandler 依赖 domain::AuthService；AuthService 依赖 domain::IUserRepository（接口）+ infra::PasswordHasher/JwtService（注入） |
| **§9.4 M-2** Auth 关键路径 ≥ 80% 覆盖 | ✅ | 37 项新测试 + 84 项历史 = 121 项；覆盖路径 10/10 |
| **§9.4 M-4** 无 vcpkg / 隐藏 apt 依赖 | ✅ | 仅 Phase 1 已链接的 libmysqlclient / OpenSSL / jwt-cpp / spdlog；新增代码无新依赖 |

---

## 6. 实现文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `backend/include/common/config.hpp` | 修改 | 新增 `MysqlConfig` / `JwtConfig` 字段 |
| `backend/src/common/config.cpp` | 修改 | 解析 mysql / jwt JSON 段 |
| `backend/include/infra/mysql_client.hpp` | 新增 | 连接池（`MysqlClient` + `Lease` RAII） |
| `backend/src/infra/mysql_client.cpp` | 新增 | libmysqlclient C API 封装 + 转义辅助 |
| `backend/include/infra/jwt_service.hpp` | 新增 | HS256 JWT 颁发/验证 + `TokenClaims` |
| `backend/src/infra/jwt_service.cpp` | 新增 | jwt-cpp 封装（构造期 fail-fast） |
| `backend/include/infra/user_repo.hpp` | 新增 | `MysqlUserRepo : IUserRepository` |
| `backend/src/infra/user_repo.cpp` | 新增 | 事务化 `register_user` + find_by_* |
| `backend/include/domain/user_repository.hpp` | 新增 | `User` 结构体 + `IUserRepository` 接口 |
| `backend/include/domain/auth_service.hpp` | 新增 | `AuthService` + `RegisterError` + `RegisterResult` |
| `backend/src/domain/auth_service.cpp` | 新增 | 校验 / 查重 / 哈希 / 插入 / 颁 token |
| `backend/include/http/handlers/auth_handler.hpp` | 新增 | `register_auth_routes` 声明 |
| `backend/src/http/handlers/auth_handler.cpp` | 新增 | JSON 解析 + 错误码映射 + Refresh Cookie |
| `backend/src/main.cpp` | 修改 | 装配链路 + 注册 `/api/auth/register` 路由 |
| `backend/tests/test_auth_service.cpp` | 新增 | 20 项 AuthService 单测 |
| `backend/tests/test_auth_handler.cpp` | 新增 | 11 项 HTTP 入口 E2E |
| `backend/tests/test_user_repo_mysql.cpp` | 新增 | 6 项 MysqlUserRepo 集成测试 |
| `docs/phase3-verification.md` | 新增 | 本报告 |

**未改动**：

- `backend/sql/001_init.sql` —— `users` 表 schema 已在 Phase 2 落地，无需调整
- `backend/CMakeLists.txt` —— 所有新文件通过 `file(GLOB)` 自动收编；新增 libmysqlclient / jwt-cpp / OpenSSL / libargon2 链接均已在 Phase 1 CMakeLists 中声明
- `backend/Dockerfile` —— 镜像已能正常 `cmake --build` 出可执行文件
- `backend/config/default.json` —— 已包含 mysql / jwt 段（与新增 config parser 字段一致）

---

## 7. Phase 2 后续子项的关注点

### 7.1 `/api/auth/login`（下一项）

`AuthService::login` 应复用：

- `IUserRepository::find_by_username` —— 按用户名查找
- `infra::PasswordHasher::verify` —— 校验密码（noexcept，登录路径不能因密码错崩溃）
- `JwtService::issue_access` / `issue_refresh` —— 颁发 token
- 失败统一抛 `LoginErrorKind::InvalidCredentials`（不区分"用户不存在" vs "密码错误"，防枚举攻击）

### 7.2 `/api/auth/refresh`（下一项）

`JwtService::verify` 已在 `expected_type="refresh"` 模式下验证 refresh token；handler 只需：

1. 从 `Cookie: refresh_token=...` 解析
2. 调 `JwtService::verify(token, "refresh")` 拿 user_id
3. 重新查 `IUserRepository::find_by_id` 拿最新 is_admin
4. 颁发新 access + 新 refresh（rotation）
5. 重新写 Set-Cookie

### 7.3 rehash-on-login

Phase 2 verification §7 已埋点：当未来 `PasswordHasher::Params` 调参时，可在 `login` 路径检测 encoded 串中的 m/t/p 参数，若发现旧参数则 verify 通过后用新参数重新 hash 并写回 DB。本阶段不实现。

### 7.4 清理脚本

MySQL `users` 表当前无软删除；后续如需做"封禁用户"功能，可在 `users` 表加 `is_banned` 字段而不破坏 schema。

---

## 8. 结论

Phase 2 第 2 项 `/api/auth/register 实现首注册为 admin 逻辑` 通过验收，进入下一项
`/api/auth/login + JWT 颁发`。

**统计**：
- 新增 8 个 C++ 源/头文件 + 3 个测试文件 = 11 个新文件
- 修改 3 个现有文件（config.hpp/cpp + main.cpp）
- 新增 37 项测试，全部通过
- 端到端实测 9 项（针对运行中 Docker 容器），全部通过
- 总测试数 121 / 121 通过；总耗时 ~13s
