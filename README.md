# OnlineJudge

仿 LeetCode 风格的在线评测系统 (C++20 + 原生 HTML/CSS/JS + Docker 沙箱)。

> 详细需求见 [SPEC.md](./SPEC.md)（唯一基线）。  
> 任务拆解见 SPEC §8 TODO 清单。  
> Phase 验收报告见 [`docs/`](./docs/)。

## 当前进度

**Phase 1 – 6 全部完成 ✅**；Phase 7（打磨与验收）进行中——已完成 spdlog / 统一错误中间件 / 本 README，
剩余端到端验收（SPEC §9.1 / §9.2 全套）按计划推进。

`docker compose up -d --build` 一次成功 ~30 s（warm cache），mysql / backend / frontend 均 healthy，
`/api/health` 返回标准信封格式，5 种语言判题镜像就绪。

| Phase | 交付 | 验收报告 |
|---|---|---|
| 1 — 基础骨架 | 仓库 + Docker Compose 一键启动 | [`docs/phase1-verification.md`](./docs/phase1-verification.md) |
| 2 — 账户系统 | users / Argon2id + 首注册 admin | [`docs/phase2-verification.md`](./docs/phase2-verification.md)、[`docs/phase3-verification.md`](./docs/phase3-verification.md) |
| 3 — 题目模块 | problems / tags / 分页过滤 / Monaco | （无独立报告，含在 phase6-2） |
| 4 — 判题子系统 | judge 工具 + 5 语言镜像 + DockerClient | （含在 phase6-2） |
| 5 — 后台管理 | admin CRUD + 测试点动态增删 | [`docs/phase5-verification.md`](./docs/phase5-verification.md) |
| 6 — 提交历史 + 详情 | 个人/公共列表 + 错点 diff | [`docs/phase6-1-verification.md`](./docs/phase6-1-verification.md)、[`docs/phase6-2-verification.md`](./docs/phase6-2-verification.md) |
| 7 — 打磨与验收 | spdlog ✅ / 错误中间件 ✅ / 单元测试 ✅ / **README** ✅ / 端到端 ⏳ | [`docs/phase7-verification.md`](./docs/phase7-verification.md)、[`docs/phase7-2-verification.md`](./docs/phase7-2-verification.md)、[`docs/phase7-3-verification.md`](./docs/phase7-3-verification.md) |

---

## 仓库结构

```
onlinejudge/
├── SPEC.md                ← 需求与设计基线
├── README.md              ← 本文件
├── docker-compose.yml     ← 一键启动
├── dependence.md          ← 宿主机开发环境一键安装脚本
├── .gitignore
├── .dockerignore
│
├── backend/               ← C++20 后端 (Http / Domain / Infra / Common 四层)
├── frontend/              ← 原生 HTML / CSS / JS (History API SPA)
├── judge-images/          ← 5 种语言的判题沙箱镜像
├── judge-tool/            ← 容器内判题工具源码 (静态编译进各镜像)
├── nginx/                 ← 前端反向代理 (SPA fallback + /api 反代)
└── docs/                  ← 各 Phase 验收报告
```

## 技术栈

| 层 | 选型 | 说明 |
|---|---|---|
| 后端语言 | C++20 | gcc-13，依赖通过 CMake FetchContent / apt 安装 |
| HTTP | cpp-httplib | 头文件库，单进程即可跑 |
| 异步 I/O | libcurl | 调用 Docker Engine REST API（HTTP over UNIX socket） |
| 数据库 | MySQL 8.0 + libmysqlclient | 连接池（≥ 8）+ RAII Lease |
| 密码哈希 | libargon2 | Argon2id，PHC 编码存储于 `users.password_hash` |
| JWT | jwt-cpp | HS256，Access 2h + Refresh 7d（HttpOnly Cookie） |
| JSON | nlohmann/json | 服务端 JSON 序列化 |
| 日志 | spdlog | 文件轮转 100 MB × 10 份 + stdout |
| 前端 | 原生 HTML/CSS/JS | 无构建工具，History API SPA，深色主题 |
| 编辑器 | Monaco Editor 0.45（CDN） | 多语言、只读模式、草稿自动保存 |
| Markdown | markdown-it（CDN） | 题面渲染 + 后台编辑实时预览 |
| 反代 | nginx:alpine | SPA fallback + `/api` 反代 + CSP/安全响应头 |
| 判题沙箱 | Docker Engine API | 5 个一次性容器，`network=none` + `cap-drop ALL` + rlimit |
| 编排 | Docker Compose v2 | 一键启停 mysql / backend / frontend + judges profile |

---

## 一键启动

```bash
git clone <repo>
cd onlinejudge

# 1) 启动核心服务 (mysql / backend / frontend)
docker compose up -d --build

# 2) 构建 5 个判题镜像 (Phase 4)
docker compose --profile judges build

# 3) 浏览器访问
open http://localhost
```

### 端口

| 端口 | 服务 | 说明 |
|---|---|---|
| 80   | frontend (nginx) | 浏览器主入口 |
| 8080 | backend 直连     | 调试 / 移动端 |
| 3306 | MySQL            | 仅 docker 内部 |

### 验证安装（SPEC §9.1 AC-1 / AC-2）

启动后依次跑下面 4 条命令，全部预期符合即说明一键启动通过：

```bash
# 1) 所有服务 healthy（STATUS 列应为 'Up (healthy)'）
docker compose ps

# 2) 后端 /api/health 返回标准信封 {code:0, message:"ok", data:{...}}
curl -s http://localhost:8080/api/health | jq .

# 3) 经 nginx 反代同样可访问
curl -s http://localhost/api/health | jq .

# 4) 浏览器访问 http://localhost 首页能渲染（DOM 不报 JS 错）
curl -sI http://localhost/ | head -1      # → HTTP/1.1 200 OK
```

若任一步骤失败，先看日志：
```bash
docker compose logs -f --tail=200 mysql backend frontend
```

判题镜像验证（AC-8 关键路径）：
```bash
docker compose --profile judges build           # 一次性构建 5 个判题镜像
./judge-images/smoke.sh                        # AC + TLE 双覆盖 smoke
# 预期: pass=10  fail=0   （每语言 2 个用例）
```

---

## 本地开发

### 5 步上手 (推荐顺序)

```bash
# 0) 安装依赖 (一次性, Debian/Ubuntu)
sudo apt-get install -y g++ cmake ninja-build pkg-config \
  default-libmysqlclient-dev libcurl4-openssl-dev libargon2-dev \
  default-mysql-client default-jre

# 1) 拉起基础设施
docker compose up -d mysql           # 仅 mysql,后端单独编译跑

# 2) 编译后端 (CMake 推荐方式)
cd backend
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

# 3) 跑后端(直连 8080)
./build/oj_backend --config config/default.json

# 4) 跑前端(任意静态服务器均可,推荐直接用 docker compose 的 frontend)
cd ../
docker compose up -d frontend        # 自动反代 backend:8080

# 5) 浏览器访问 http://localhost
```

### 宿主机编译后端 (Phase 7 单元测试)

参见 [`dependence.md`](./dependence.md) 一次性安装:
- `g++` (C++20) / `cmake` / `ninja-build` / `pkg-config`
- `default-libmysqlclient-dev` / `libcurl4-openssl-dev` / `libargon2-dev`
- `default-mysql-client`

```bash
cd backend
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/oj_backend --config config/default.json
```

#### 跑单元测试 (Phase 7)

```bash
cd backend
cmake --build build -j
./build/oj_unit_tests                          # 全部 (MySQL 类默认 SKIP)
./build/oj_unit_tests --gtest_filter='*Auth*'  # 只跑 Auth 相关
```

> 当前规模 (Phase 7.3): **728 项单元测试 / 81 个 suite 全部 PASS**
> (含 99 项 MySQL 集成；详见 [`docs/phase7-3-verification.md`](./docs/phase7-3-verification.md))。
> 跑全套需先启动 docker compose 的 mysql 容器:
> ```bash
> docker compose up -d mysql
> docker inspect oj_mysql -f '{{.NetworkSettings.Networks.oj_internal.IPAddress}}'
> # 假设输出 172.21.0.2
> OJ_RUN_MYSQL_TESTS=1 OJ_MYSQL_HOST=172.21.0.2 ./build/oj_unit_tests
> ```
> 按域过滤: `--gtest_filter='*Problem*:*Admin*'` / `*Judge*:*Submission*:*Docker*`

#### 离线 / 内网构建 (可选)

默认 CMake 会通过 FetchContent 从 GitHub 拉 `cpp-httplib` / `spdlog` / `jwt-cpp` / `googletest` / `nlohmann_json`。
如果在内网或断网环境，把这些源码 clone 到 `backend/.local-deps/` 下，CMake 会自动走本地路径、不再访问外网:

```bash
cd backend
mkdir -p .local-deps && cd .local-deps
for repo in \
  "https://github.com/yhirose/cpp-httplib.git|v0.15.3|cpp-httplib" \
  "https://github.com/gabime/spdlog.git|v1.13.0|spdlog" \
  "https://github.com/Thalhammer/jwt-cpp.git|v0.7.0|jwt-cpp" \
  "https://github.com/google/googletest.git|v1.15.2|googletest" \
  "https://github.com/nlohmann/json.git|v3.11.3|json"; do
  IFS='|' read -r url tag dir <<< "$repo"
  git clone --depth=1 --branch "$tag" "$url" "$dir"
done
cd .. && cmake -S . -B build -G Ninja   # 自动检测 .local-deps/
```

`backend/.local-deps/` 已被 `.gitignore` 忽略，不会进仓库。

---

## 部署

### 全新机器 5 分钟启动 (SPEC §1.4 AC-1)

```bash
git clone <repo>
cd onlinejudge
docker compose up -d --build          # ~30s warm cache / ~5min cold
```

### 端到端验证清单（SPEC §9.1 AC 速查）

一键启动完成后，按顺序执行即可验证"注册 → 出题 → 做题 → 提交 → AC"完整链路：

| AC# | 验证项 | 操作 |
|---|---|---|
| AC-1 | 5 分钟启动 | `time docker compose up -d --build` |
| AC-2 | 首页可访问 | 浏览器打开 `http://localhost` |
| AC-3 | 首注册为 admin | 注册账号 A → 退出 → 注册账号 B → B 进入后台被拒 |
| AC-4 | 参数/唯一性校验 | 重复用户名 / 密码 < 8 / 邮箱非法 → 1005 / 1001 |
| AC-5 | 后台出题 | 用 A 登录 → `/admin/problems/new` → 创建"两数之和"题 |
| AC-6 | 测试点总分 ≠ 100 禁用 | 在 admin 表单 score 之和 ≠ 100 → [保存] disabled |
| AC-7 | 未发布不可见 | B 用普通账号登录 → `/problems` 看不到草稿 |
| AC-8 | AC 提交 | 用 B 在题详情页提交正确代码 → result=AC, score=100 |
| AC-9 | TLE | 提交 `while(1){}` → result=TLE |
| AC-10 | WA | 提交 `printf("0\n")` → result=WA |
| AC-11 | CE | C++ 故意写错语法 → result=CE，查看 compile_output |
| AC-12 | RE | 提交除以 0 的代码 → result=RE |
| AC-13 | while 死循环不挂死 | Java/Python/C++ 各自跑 `while(1){}` → TLE |
| AC-14 | fork bomb 不挂死 | shell 容器不存在；提交层已 network=none + pids-limit |
| AC-15 | 无网络 | 代码 `curl http://example.com` → RE/TLE |
| AC-16 | 只读根文件系统 | 代码读 `/etc/passwd` → 容器只读失败 |
| AC-17 | 提交列表分页 | `/submissions` 翻页正确 |
| AC-18 | AC 公开 | B 的 AC 提交对匿名用户可见，WA 提交仅自己可见 |
| AC-19 | 错点详情 | WA 提交详情页点 [查看] → diff 弹窗，样例点 3 列、隐藏点占位 |
| AC-20 | 50 并发 | `seq 50 \| xargs -I{} curl ...` 同一份 AC 代码 → 全 AC |
| AC-21 | 排队 | 50 并发时第 5 个进入 queued，2s 内被 worker 拾取 |
| AC-22 | MySQL 故障 | `docker compose stop mysql` → API 返回 503；`start mysql` 后恢复 |

性能 / 安全 / 可维护性验收（SPEC §9.2 / §9.3 / §9.4）见 `docs/phase7-verification.md`。

### 端口与网络

- 对外: 80 (前端) + 8080 (后端直连)
- 内部: MySQL 3306 仅 backend 可见
- 判题容器: `network_mode: none`,与任何网络隔离 (SPEC §3.4)

### 生产部署建议

1. **HTTPS**: 在前端前置一层 nginx (含 SSL/TLS 终止) ,后端仍走 8080 HTTP 内部网;
   或直接放云负载均衡器 (ALB / CLB) 上做 TLS 终止。
2. **JWT secret**: 务必替换 `backend/config/default.json` 里
   `jwt.secret` 的 32+ 字节随机值;推荐用 Vault / K8s Secret 注入。
3. **MySQL**: 把 `mysql_data` 卷挂到 SSD;为生产数据配置独立备份 (mysqldump 或 binlog)。
4. **judge 镜像**: 跑 `docker compose --profile judges build` 一次性构建,
   后续 docker compose up 会复用本地镜像,不再拉取。
5. **资源**: SPEC §2.6 — 单机 ≤ 50 并发提交,判题线程池固定 4 个;CPU ≥ 4 核,
   内存 ≥ 8 GB (mysql 1G + backend 1G + 5 个判题容器峰值 1.3G)。
6. **日志**: backend 通过 spdlog 写到 `backend_logs` 卷 (`/var/log/oj`),
   容器 stdout 走 docker 默认 json-file driver (上限 100MB × 5 份,见 `docker-compose.yml` `x-logging`)。
7. **健康检查**: backend 的 docker HEALTHCHECK 每 15s 探 `/api/health`,
   MySQL 的 docker HEALTHCHECK 每 10s 探 `mysqladmin ping`,
   frontend 的 docker HEALTHCHECK 每 15s 探 `wget /`。

### 升级流程

```bash
git pull
docker compose pull                     # 拉新 base image
docker compose build                    # 重新构建 backend + judges
docker compose up -d                    # 滚动重启
docker compose logs -f backend          # 确认无启动错误
```

判题容器 (judge-cpp 等) 是按需创建的临时容器,不需要单独升级 ——
只要新版本镜像构建好,backend 自动会用新版本。

### 回滚

```bash
docker compose down                     # 停服务
git checkout <last-good-commit>
docker compose build
docker compose up -d
```

数据库 schema 变更通过 `backend/sql/00*_*.sql` 顺序执行 (`001_init.sql` + `002_seed.sql`)。
生产环境建议手工跑 `ALTER TABLE` 而非重跑 init,以保留数据。

---

## 常见问题

**Q: 启动后 `http://localhost` 报 502 Bad Gateway?**  
A: backend 还没 ready。等 ~20s 后重试 (backend HEALTHCHECK start_period=20s)。
也可 `docker compose logs -f backend` 看启动日志。

**Q: 提交代码后长时间停在 "判题中" 而不出结果?**  
A: 检查 backend 日志和 docker daemon 状态:
```bash
docker compose logs -f backend         # 看 judge worker 有无报错
docker ps                               # 看 judge-* 容器是否被创建
ls /var/lib/docker/volumes/onlinejudge_judge_work  # 看工作目录
```
如果 backend 日志提示 "judge container create failed",需要重新构建 judge 镜像:
```bash
docker compose --profile judges build
```

**Q: 想清空所有数据重新开始?**  
```bash
docker compose down -v
docker compose up -d --build
```

**Q: 想直连后端调试 (绕开 nginx)?**  
```bash
curl http://localhost:8080/api/health
```

**Q: 想看后端访问日志?**  
```bash
docker compose exec backend tail -f /var/log/oj/oj_backend.log
```

**Q: 编译报错 "libmysqlclient not found"?**  
A: Debian bookworm 默认装的是 `libmariadb3` (MariaDB 兼容实现);安装 `default-libmysqlclient-dev` 即可。
Ubuntu 24.04+ 同理。

**Q: 编译报错 "spdlog not found" / "nlohmann_json not found"?**  
A: 内网环境请按上文"离线 / 内网构建"克隆依赖到 `backend/.local-deps/`。

**Q: 想单独验证 5 个判题镜像都能跑？**  
```bash
docker compose --profile judges build   # 一次性构建
./judge-images/smoke.sh                 # AC + TLE 各 5 个用例 = 10 全过
```
失败时会在 stdout 列出每个失败用例的实际 result 字段和 `summary.json` 前 10 行,
配合对应镜像的 Dockerfile / `judge-images/common/entrypoint.sh` 排查。

**Q: 想自己重新编译判题工具 (judge-tool)？**  
```bash
cd judge-tool
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 静态二进制 build/judge 已产出, 重跑 ./judge-images/build.sh 即可重新打包到各镜像
```

**Q: 跑前端单元测试？**  
```bash
cd frontend
node --check js/*.js js/**/*.js                # 语法校验
node tests/<file>.test.mjs                     # 单文件, 例:
node tests/poller.test.mjs                     # 22 项 poller 单测
node tests/problem-cases.test.mjs              # 27 项测试点校验
node tests/submission-detail-helpers.test.mjs  # 50 项 diff / LCS
```

---

## 文档

- [SPEC.md](./SPEC.md) — 完整需求、架构、接口、判题、部署
- [dependence.md](./dependence.md) — 宿主机开发环境一键安装脚本
- [`docs/phase1-verification.md`](./docs/phase1-verification.md) — Phase 1 一键启动验证报告
- [`docs/phase2-verification.md`](./docs/phase2-verification.md) — Phase 2-1 users 表 + Argon2id
- [`docs/phase3-verification.md`](./docs/phase3-verification.md) — Phase 2-2 /api/auth/register + 首注册 admin
- [`docs/phase5-verification.md`](./docs/phase5-verification.md) — Phase 5 后台题目管理
- [`docs/phase6-1-verification.md`](./docs/phase6-1-verification.md) — Phase 6-1 提交列表
- [`docs/phase6-2-verification.md`](./docs/phase6-2-verification.md) — Phase 6-2 提交详情 + 错点 diff
- [`docs/phase7-verification.md`](./docs/phase7-verification.md) — Phase 7 打磨与验收
- [`docs/phase7-2-verification.md`](./docs/phase7-2-verification.md) — Phase 7-2 统一错误中间件
- [`docs/phase7-3-verification.md`](./docs/phase7-3-verification.md) — Phase 7-3 单元测试 (Auth/Problem/Judge 728 PASS)

---

## 许可

内部教学演示项目。