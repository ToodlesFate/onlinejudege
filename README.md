# OnlineJudge

仿 LeetCode 风格的在线评测系统 (C++20 + 原生 HTML/CSS/JS + Docker 沙箱)。

> 详细需求见 [SPEC.md](./SPEC.md)（唯一基线）。  
> 任务拆解见 SPEC §8 TODO 清单。  
> Phase 验收报告见 [`docs/`](./docs/)。

## 当前进度

**Phase 1 – 6 全部完成 ✅，Phase 7（打磨与验收）✅**。
`docker compose up -d --build` 一次成功 ~30 s，mysql / backend / frontend 均 healthy，
`/api/health` 返回标准信封格式，5 种语言判题镜像就绪。

| Phase | 交付 | 验收报告 |
|---|---|---|
| 1 — 基础骨架 | 仓库 + Docker Compose 一键启动 | [`docs/phase1-verification.md`](./docs/phase1-verification.md) |
| 2 — 账户系统 | users / Argon2id + 首注册 admin | [`docs/phase2-verification.md`](./docs/phase2-verification.md)、[`docs/phase3-verification.md`](./docs/phase3-verification.md) |
| 3 — 题目模块 | problems / tags / 分页过滤 / Monaco | （无独立报告，含在 phase6-2） |
| 4 — 判题子系统 | judge 工具 + 5 语言镜像 + DockerClient | （含在 phase6-2） |
| 5 — 后台管理 | admin CRUD + 测试点动态增删 | [`docs/phase5-verification.md`](./docs/phase5-verification.md) |
| 6 — 提交历史 + 详情 | 个人/公共列表 + 错点 diff | [`docs/phase6-1-verification.md`](./docs/phase6-1-verification.md)、[`docs/phase6-2-verification.md`](./docs/phase6-2-verification.md) |
| 7 — 打磨与验收 | spdlog / 错误中间件 / 单元测试 / README | [`docs/phase7-verification.md`](./docs/phase7-verification.md) |

---

## 仓库结构

```
onlinejudge/
├── SPEC.md                ← 需求与设计基线
├── README.md              ← 本文件
├── docker-compose.yml     ← 一键启动
├── .gitignore
├── .dockerignore
│
├── backend/               ← C++20 后端 (Http / Domain / Infra / Common 四层)
├── frontend/              ← 原生 HTML / CSS / JS (History API SPA)
├── judge-images/          ← 5 种语言的判题沙箱镜像
├── mysql/initdb/          ← 首次启动自动建表 SQL
├── nginx/                 ← 前端反向代理 (SPA fallback + /api 反代)
└── docs/                  ← 各 Phase 验收报告
```

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
./build/oj_unit_tests                    # 全部
./build/oj_unit_tests --gtest_filter='*Auth*'   # 只跑 Auth 相关
```

> 当前规模 (Phase 7): **579 项单元测试**,全部通过 (不含需 MySQL 的 32 项)。
> 跑全套需先启动 docker compose 的 mysql 容器,默认 `localhost:3306`。

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

---

## 许可

内部教学演示项目。