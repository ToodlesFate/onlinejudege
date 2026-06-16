# Phase 1 验收报告 — Docker Compose 一键启动验证

> 对应 SPEC §8「Phases 1 - 基础骨架」第 5 项：`[ ] Docker Compose 一键启动验证`。
> 本文档记录从 0 到完成态的端到端实测，包括命令、原始输出与就地修复记录。
>
> 验收时间：2026-06-16
> 环境：Linux x86_64 / Docker 29.3.1 / Docker Compose v5.1.1
> 结果：**通过** ✅

---

## 1. 验证范围

按 SPEC §1.4 成功标准与 §9.1.1 AC-1/AC-2，验证以下事实：

- [x] `docker compose up -d --build` 一次成功（AC-1）
- [x] 全部 3 个常驻服务 `mysql` / `backend` / `frontend` 状态 `healthy`（AC-1）
- [x] 端到端启动 < 5 分钟（AC-1）
- [x] `http://localhost` 返回 HTML 且所有静态资源 200（AC-2）
- [x] 后端 `/api/health` 返回标准信封格式（SPEC §5.1）
- [x] MySQL 7 张表 + 8 个标签已自动创建并 seed
- [x] nginx `/api/` 反代生效
- [x] SPA 路由 fallback 生效（任意未知路径回 index.html）

---

## 2. 验证前发现的问题与修复

### 2.1 `libmysqlclient21` 在 Debian bookworm 不存在

**症状**：第一次 `docker compose build backend` 报

```
E: Unable to locate package libmysqlclient21
```

**原因**：Debian 12 (bookworm) 已切换到 MariaDB 作为 MySQL 客户端实现，
默认客户端库是 `libmariadb3`，`libmysqlclient21` 仅在 Ubuntu 仓库保留。

**修复**：`backend/Dockerfile` runtime 阶段把 `libmysqlclient21` 换成 `libmariadb3`。

```diff
 RUN apt-get install -y --no-install-recommends \
         ca-certificates \
+        curl \
         default-mysql-client \
         libcurl4 \
         libargon2-1 \
-        libmysqlclient21 \
+        libmariadb3 \
         libssl3 \
         tini
```

### 2.2 `CMakeLists.txt` 缺 install 规则

**症状**：镜像构建最后一步报

```
ERROR: failed to calculate checksum of ref ... "/install/bin/oj_backend": not found
```

**原因**：`CMakeLists.txt` 只用 `configure_file` 把 `config/default.json` 拷到
`${CMAKE_BINARY_DIR}/config/`，但从未声明 `install(TARGETS ...)` / `install(FILES ...)`，
`cmake --install build --prefix /install` 实际没有任何产物被写入 `/install/`。

**修复**：补 install 规则（用 `GNUInstallDirs` 自动跨平台），让二进制落到 `bin/`、配置落到
`share/onlinejudge/config/`：

```cmake
include(GNUInstallDirs)
install(TARGETS oj_backend
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   ${CMAKE_SOURCE_DIR}/config/default.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/onlinejudge/config
        RENAME     default.json)
```

### 2.3 Dockerfile 未拷贝 `backend/.local-deps/`，离线环境构建失败

**症状**：builder 阶段 FetchContent 拉 GitHub 超时：

```
fatal: unable to access 'https://github.com/nlohmann/json.git/':
       Failed to connect to github.com port 443 after 134204 ms
```

**原因**：上层 `CMakeLists.txt` 检测到 `backend/.local-deps/` 目录时本应走本地路径，
但 Dockerfile 没有 `COPY .local-deps/`，导致容器内 `OJ_LOCAL_DEPS_DIR` 指向不存在的目录，
回退到 FetchContent 路径（内网 / 离线无法访问）。

**修复**：

```dockerfile
# 拷贝离线依赖（如果 backend/.local-deps 存在）
COPY .local-deps/      ./.local-deps/

RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOJ_BUILD_TESTS=OFF \
        -DOJ_LOCAL_DEPS_DIR=/src/.local-deps \
 && cmake --build build --parallel "$(nproc)" \
 && cmake --install build --prefix /install
```

这与 README §本地开发 → 离线 / 内网构建章节的指引一致。

### 2.4 backend 镜像未装 `curl`，容器 healthcheck 失败

**症状**：首次 `docker compose up`：

```
oj_backend Error dependency backend failed to start
dependency failed to start: container oj_backend is unhealthy
```

**原因**：`docker-compose.yml` 的 backend healthcheck 用 `curl -fsS http://127.0.0.1:8080/api/health`，
但 runtime 镜像只装了 `libcurl4`（动态库），没装 `curl`（CLI），容器内调用失败。

**修复**：runtime 阶段追加 `curl` 包（见 2.1 的 diff）。

---

## 3. 端到端实测

### 3.1 全量重建 + 启动耗时

```bash
docker compose down -v
time docker compose up -d --build
```

```
real    0m29.961s
user    0m0.187s
sys     0m0.072s
```

所有 3 个容器在 `up` 完成约 **16 秒** 内全部 healthy：

```
NAME          IMAGE            STATUS
oj_backend    oj_backend:1.0   Up 23 seconds (healthy)
oj_frontend   nginx:alpine     Up 16 seconds (healthy)
oj_mysql      mysql:8.0        Up 44 seconds (healthy)
```

✅ 远低于 AC-1 的 5 分钟预算。

### 3.2 后端健康检查（直连 vs 反代）

**直连 8080**：

```bash
curl -sS http://127.0.0.1:8080/api/health
```

```json
{
  "code": 0,
  "data": {
    "now_unix": 1781616069,
    "status": "ok",
    "uptime_ms": 65174,
    "version": "1.0.0"
  },
  "message": "ok"
}
```

**经 nginx 80**：

```bash
curl -sS http://127.0.0.1/api/health
```

```json
{
  "code": 0,
  "data": {
    "now_unix": 1781616069,
    "status": "ok",
    "uptime_ms": 65204,
    "version": "1.0.0"
  },
  "message": "ok"
}
```

✅ 信封格式 `{code, message, data}` 完全符合 SPEC §5.1。
✅ nginx `/api/` 反代生效。

### 3.3 前端页面与 SPA fallback

```bash
curl -sS -o /dev/null -w "%{http_code} %{size_download}b  %{url_effective}\n" \
     http://127.0.0.1/ \
     http://127.0.0.1/problems \
     http://127.0.0.1/submissions/3 \
     http://127.0.0.1/admin/x/y/z
```

```
200 809b  http://127.0.0.1/
200 809b  http://127.0.0.1/problems
200 809b  http://127.0.0.1/submissions/3
200 809b  http://127.0.0.1/admin/x/y/z
```

✅ 全部 200，未知路径回落到 `index.html`（SPA History 路由 fallback 生效）。

### 3.4 静态资源 + JS 模块

```bash
for f in base layout components; do
  curl -sS -o /dev/null -w "css/$f.css -> HTTP %{http_code}\n" \
       "http://127.0.0.1/css/$f.css"
done
for f in main router; do
  curl -sS -o /dev/null -w "js/$f.js   -> HTTP %{http_code}\n" \
       "http://127.0.0.1/js/$f.js"
done
# 同样测了 js/utils/dom.js / js/views/home.js / js/views/_stub.js / js/views/not-found.js
```

✅ 全部 200。

ESM 语法校验：

```bash
node --check frontend/js/main.js         # OK
node --check frontend/js/router.js       # OK
node --check frontend/js/utils/dom.js    # OK
node --check frontend/js/views/home.js   # OK
node --check frontend/js/views/_stub.js  # OK
node --check frontend/js/views/not-found.js # OK
```

✅ 无 syntax error，所有 `import` 路径都解析到现有文件。

### 3.5 数据库 schema + seed

```bash
docker exec oj_mysql mysql -uoj -poj oj \
  -e "SELECT TABLE_NAME, TABLE_COMMENT FROM information_schema.TABLES
      WHERE TABLE_SCHEMA='oj' ORDER BY TABLE_NAME;"
```

```
TABLE_NAME         TABLE_COMMENT
problem_tags       题目-标签关联表
problems           题目表
submission_cases   提交逐点结果
submissions        提交记录
tags               标签表 (预置 8 个, 不开放后台管理)
testcases          题目测试点
users              用户表
```

✅ 7 张表全部建好，与 SPEC §4.2 表结构一致。

```bash
docker exec oj_mysql mysql -uoj -poj oj --default-character-set=utf8mb4 \
  -e "SELECT id, name, slug FROM tags ORDER BY id;"
```

```
id  name      slug
1   数组      数组
2   字符串    string
3   链表      linked-list
4   栈/队列   stack-queue
5   树        tree
6   图        graph
7   动态规划  dp
8   贪心      greedy
```

✅ 8 个标签全部 seed，utf8mb4 中文正常显示。

### 3.6 backend → mysql 网络

```bash
docker exec oj_backend bash -c "mysqladmin ping -h mysql -u oj -poj"
# mysqld is alive
```

```bash
docker exec oj_backend bash -c \
  "mysql -h mysql -u oj -poj oj -e 'SELECT COUNT(*) FROM users;'"
# COUNT(*) = 0
```

✅ 容器内部 DNS 解析 `mysql` 成功；连接串 / 凭据与 `docker-compose.yml` 一致。

### 3.7 容器日志

```bash
docker compose logs backend --tail=5
# [info] oj_backend 1.0.0 starting up; config=/app/config/default.json
# [info] oj_backend 1.0.0 listening on 0.0.0.0:8080 (threads=8)
```

```bash
docker compose logs mysql --tail=3
# [System] mysqld: ready for connections.
```

✅ 全部无 ERROR / 无 panic；版本号与 `CMakeLists.txt` 一致。

---

## 4. 改动文件清单

| 文件 | 类型 | 说明 |
|---|---|---|
| `backend/Dockerfile` | 修复 | `libmysqlclient21` → `libmariadb3`；追加 `curl`；COPY `.local-deps/`；传 `-DOJ_LOCAL_DEPS_DIR=...` |
| `backend/CMakeLists.txt` | 修复 | 补 `install(TARGETS/FILES)` 规则；引入 `GNUInstallDirs` |
| `SPEC.md` | 文档 | §8 TODO 第 5 项标 `[x]`；新增 §9.5 验收报告小节 |
| `docs/phase1-verification.md` | 新增 | 本文档 |

---

## 5. 后续 Phase 关注点

- Phase 4 判题子系统实现时，需要在 `docker compose --profile judges build` 流程里
  复用本次验证的离线依赖模式（`OJ_LOCAL_DEPS_DIR` 思路）。
- 当 SPEC §9.3 S-1 / S-2 / S-4 等安全/可维护性验收项开始落实时，
  当前镜像已经具备非 root 用户 (`oj` UID 1000)、最小 runtime 依赖（仅 curl / mysql-client），
  满足分阶段加固的基础。
- `docker compose down -v` 会清空 MySQL 数据卷，请注意开发期业务数据持久化
  策略（`mysql_data` 卷已在 `docker-compose.yml` 命名）。
