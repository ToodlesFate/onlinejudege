# OnlineJudge

仿 LeetCode 风格的在线评测系统。

> 详细需求见 [SPEC.md](./SPEC.md)（唯一基线）。  
> 任务拆解见 SPEC §8 TODO 清单。

## 当前进度

**Phase 1 — 基础骨架** 进行中（仓库初始化、目录结构、CMakeLists、Dockerfile、docker-compose.yml）。

## 仓库结构

```
onlinejudge/
├── SPEC.md                ← 需求与设计基线
├── README.md              ← 本文件
├── docker-compose.yml     ← 一键启动
├── .gitignore
├── .dockerignore
│
├── backend/               ← C++20 后端（Http / Domain / Infra / Common 四层）
├── frontend/              ← 原生 HTML / CSS / JS（History API SPA）
├── judge-images/          ← 5 种语言的判题沙箱镜像
├── mysql/initdb/          ← 首次启动自动建表 SQL
└── nginx/                 ← 前端反向代理（SPA fallback + /api 反代）
```

## 一键启动

```bash
git clone <repo>
cd onlinejudge

# 1) 启动核心服务（mysql / backend / frontend）
docker compose up -d --build

# 2) 单独构建 5 个判题镜像（Phase 4 才需要）
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

## 本地开发

### 宿主机编译后端（可选，推荐仅做 Phase 7 的单元测试开发）

参见 [dependence.md](./dependence.md) 一次性安装：
- `g++` (C++20) / `cmake` / `ninja-build` / `pkg-config`
- `default-libmysqlclient-dev` / `libcurl4-openssl-dev` / `libargon2-dev`
- `default-mysql-client`

```bash
cd backend
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/oj_backend --config config/default.json
```

> 注：当前 Phase 1 的 `src/` 仅有占位骨架，`cmake --build` 会成功（产物为空 main），但 `oj_backend` 还无法真正启动服务 —— C++ 代码在后续 todo 中补全。

## 文档

- [SPEC.md](./SPEC.md) — 完整需求、架构、接口、判题、部署
- [dependence.md](./dependence.md) — 宿主机开发环境一键安装脚本
- `docs/` — 后续补充：architecture / api / dev-guide

## 许可

内部教学演示项目。
