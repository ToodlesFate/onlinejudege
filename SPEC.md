# OnlineJudge — 规格说明书 (SPEC)

> 文档版本：v1.0  
> 适用范围：仿 LeetCode 风格的在线评测系统（OJ）  
> 技术栈：C++20 后端（cpp-httplib）+ 原生 HTML/CSS/JS（无构建）  
> 部署形态：Docker Compose 一键启动

---

## 0. 文档说明

本文档为 OJ 项目的**唯一需求与设计基线**，所有代码实现、测试用例、验收必须以本文档为准。  
若实现过程中出现需求变更，需先更新本文档，再修改代码。

阅读指引：
- 第 1 章：项目目标与边界
- 第 2 章：功能 / 非功能需求
- 第 3 章：架构设计（含架构图、前端页面规格）
- 第 4 章：数据模型
- 第 5 章：接口契约
- 第 6 章：判题子系统
- 第 7 章：部署
- 第 8 章：TODO 清单
- 第 9 章：验收标准

---

## 1. 项目目标与边界

### 1.1 项目定位
- **类型**：个人 / 教学演示项目
- **目标用户**：算法学习者与课堂师生
- **运行规模**：单台服务器，支持 **≤ 50 并发提交**
- **核心价值**：完整跑通"用户注册 → 做题 → 提交 → 自动判分 → 查看结果"端到端流程，演示 5 种主流语言在 Docker 沙箱中的安全评测

### 1.2 范围内（In Scope）
- 用户注册、登录、Token 管理
- 题目列表、详情、提交、判题、结果查看
- 后台管理：题目录入、测试点管理
- 5 种语言：C / C++ / Java / Python / Go
- 中文界面（仅深色主题）
- Docker Compose 一键启动

### 1.3 范围外（Out of Scope）
- 防作弊、代码查重
- 提交频率限流
- 讨论区、评论
- 双语 / 多主题
- SPJ（Special Judge）
- 排行榜、比赛模式
- 邮件 / 短信通知
- 第三方登录
- K8s / 微服务部署

### 1.4 成功标准
满足以下全部条件视为 v1.0 达成：
1. 全新机器 `docker compose up` 即可在 5 分钟内启动全部服务
2. 完成"注册→出题→做题→提交→AC"端到端流程
3. 5 种语言均可正常判题
4. 通过第 9 章所有验收用例

---

## 2. 需求规格

### 2.1 用户与权限

| 角色 | 权限 |
|---|---|
| **游客** | 浏览题目列表、查看题目详情、查看公开提交（仅 AC） |
| **普通用户** | 注册登录、提交代码、查看自己的全部提交、查看公开提交 |
| **管理员 (admin)** | 普通用户权限 + 题目录入 / 编辑 / 删除 / 上下架 + 用户角色管理 |

**注册机制**：
- 开放注册，无需邀请码
- **首个成功注册的账号自动成为 admin**（由应用启动时通过 `is_first_user` 标志判断）
- 注册字段：用户名（唯一，3–20 字符）、邮箱（唯一）、密码（≥ 8 字符）
- 密码哈希：Argon2id（`argon2` 库）
- 注册后立即返回 JWT 令牌对（Access + Refresh）

**Token 生命周期**：
- Access Token：有效期 2 小时，存放于 `Authorization: Bearer <token>` Header
- Refresh Token：有效期 7 天，存放于 HttpOnly Cookie（`refresh_token`）
- 静默刷新：前端检测 Access Token 即将过期时调用 `/api/auth/refresh`，后端颁发新 Access Token 并轮换 Refresh Token

### 2.2 题目模块

#### 2.2.1 题目数据模型
每道题目包含：
- 标题（≤ 100 字符）
- 题面（Markdown，≤ 64 KB）
- 难度：`easy` / `medium` / `hard`（3 级，预定义）
- 标签：多选（预置 8 个：`数组`、`字符串`、`链表`、`栈/队列`、`树`、`图`、`动态规划`、`贪心`）
- 时间限制：默认 2 秒（可逐题覆写，范围 1–10 秒）
- 内存限制：默认 256 MB（可逐题覆写，范围 64–1024 MB）
- 输出限制：默认 64 MB（可逐题覆写，范围 1–256 MB）
- 测试点：1–100 个，每个含 `input` / `expected_output` / `is_sample` / `score` 字段
- 总分 100 分（各测试点 score 之和必须等于 100）

#### 2.2.2 题目列表
- 分页：默认每页 20 条，可选 10/20/50
- 过滤：按难度、按标签（多选 AND）
- 排序：按 ID / 按通过率 / 按创建时间
- 每条目显示：标题、难度、标签、通过率（仅 admin 可见 "已通过人数/总提交数"）

#### 2.2.3 题目详情
- 渲染 Markdown 题面
- 展示样例测试点（`is_sample = true`）
- 提供 Monaco 代码编辑器（默认 C++）
- 提供"提交"按钮

### 2.3 提交与判题

#### 2.3.1 提交流程
1. 用户在编辑器中选择语言、编写代码、点击"提交"
2. 前端 POST `/api/submissions`，body 包含 `problem_id`、`language`、`code`
3. 后端校验（代码 ≤ 64 KB，题目存在），创建 submission 记录（status = `queued`），返回 `submission_id`
4. 前端启动轮询（详见 2.3.4）
5. Judge Worker 拾取任务，执行判题
6. 前端通过轮询获取终态结果

#### 2.3.2 判题状态机
```
queued ──> compiling ──> running ──> finished (one of 8 terminal states)
                              │
                              └──> finished (early-exit on CE/SE)
```

**8 种终态**：
| 状态码 | 含义 | 触发条件 |
|---|---|---|
| `AC` | Accepted | 所有测试点通过 |
| `WA` | Wrong Answer | 至少一个测试点输出不匹配 |
| `TLE` | Time Limit Exceeded | 任一测试点耗时 > 时间限制 |
| `MLE` | Memory Limit Exceeded | 任一测试点内存 > 内存限制 |
| `OLE` | Output Limit Exceeded | 任一测试点输出字节数 > 输出限制 |
| `RE` | Runtime Error | 进程非 0 退出码或信号 |
| `CE` | Compile Error | 编译阶段失败 |
| `SE` | System Error | 系统级异常（Docker 启动失败、判题机内部错误） |

**判题任务表（submissions 表）核心字段**：
- `id` BIGINT 主键
- `user_id` 提交用户
- `problem_id` 题目
- `language` 6 选 1
- `code` MEDIUMTEXT
- `status` 枚举（`queued` / `compiling` / `running` / `finished`）
- `result` 8 态之一（status=finished 后填）
- `total_score` 0–100
- `time_used` 整道题总耗时 ms（最慢测试点）
- `memory_used` 整道题总内存 KB（峰值测试点）
- `compile_output` MEDIUMTEXT（CE 时填）
- `judge_message` VARCHAR(500)（SE 时填）
- `created_at` / `finished_at` 时间戳

**测试点结果表（submission_cases）**：
- `submission_id` 外键
- `case_index` 序号
- `status` 8 态之一（单点状态）
- `time_used` ms
- `memory_used` KB
- `score` 获得的分数（0 或该点满分）
- `is_sample` 是否样例点
- `user_output` MEDIUMTEXT（**仅当 is_sample = true 时返回**，隐藏点不返内容）

#### 2.3.3 判题协议（容器内）
详见第 6 章。

#### 2.3.4 前端轮询
- 提交成功后前端每 **2 秒** 调用 `GET /api/submissions/{id}`
- 状态为 `finished` 时立即停止轮询
- 最多轮询 30 分钟（超时后停止并提示）
- 切换 tab 至后台时**不暂停**轮询（简化实现）

### 2.4 提交历史
- 个人提交列表：分页（20/页）、按时间倒序、可按题目 / 语言 / 状态过滤
- 公开提交列表：仅展示 AC 状态、可分页
- 提交详情：源代码（Monaco 只读模式）、总状态、总分、总耗时、总内存、逐测试点状态表格
  - 错点（WA / TLE / MLE / OLE / RE）展示该点 diff（用户输出 vs 预期）
  - 样例点：展示 input / expected / user_output
  - 隐藏点：仅展示 status / time / memory

### 2.5 后台管理（仅 admin）
- 题目列表（额外显示创建者、创建时间、状态）
- 题目录入表单：
  - 基本字段：标题、难度、标签（多选 checkbox）
  - 资源限制：时间 / 内存 / 输出（三档下拉 + 自定义）
  - 题面编辑器：Monaco Markdown 模式 + 实时预览
  - 测试点管理：动态增删行，每行含 input / expected_output / is_sample / score
  - "校验总分"按钮：前端校验所有 score 之和 = 100
- 题目编辑 / 删除 / 上下架（`is_published` 字段）

### 2.6 非功能性需求

| 维度 | 指标 |
|---|---|
| **性能** | 提交响应 < 200ms（不含判题）；判题端到端 < 30s（P95）；列表页加载 < 1s |
| **并发** | 同时 ≤ 50 个提交；判题线程池固定 4 个（可配置） |
| **可用性** | 单机部署，无 SLA 承诺；MySQL 不可用时返回 503，Docker 不可用时判题任务进入 SE |
| **安全** | 见 6.4 沙箱安全策略 |
| **可观测** | spdlog 文件日志（轮转 100MB × 10 份）+ stdout 容器日志；所有 HTTP 请求记录 access log（方法、路径、状态、耗时、user_id） |
| **可移植** | 仅依赖 Docker / Docker Compose；不在宿主机安装任何 C++ 库 |
| **可维护** | 代码分 Http / Domain / Infra 三层；关键类有单元测试；README 含本地开发步骤 |

---

## 3. 架构设计

### 3.1 总体架构图

```
┌────────────────────────────────────────────────────────────────────┐
│                          Browser (User)                            │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  Frontend (原生 HTML/CSS/JS, History API SPA)                │  │
│  │  ┌─────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐    │  │
│  │  │ 路由层  │  │ 视图层   │  │ API 客户端│  │ Monaco 编辑 │    │  │
│  │  └─────────┘  └──────────┘  └──────────┘  └────────────┘    │  │
│  └──────────────────────────────┬───────────────────────────────┘  │
└─────────────────────────────────┼──────────────────────────────────┘
                                  │ HTTPS (反向代理时) / HTTP
                                  │ Authorization: Bearer <jwt>
                                  ▼
┌────────────────────────────────────────────────────────────────────┐
│                  Backend Container (C++20 + cpp-httplib)          │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                    HTTP Server (cpp-httplib)                 │  │
│  │  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌────────┐  │  │
│  │  │ Auth 中间件│  │ 错误处理   │  │ 路由注册   │  │ Access │  │  │
│  │  │            │  │ 中间件     │  │            │  │  Log   │  │  │
│  │  └────────────┘  └────────────┘  └────────────┘  └────────┘  │  │
│  └──────────────────────────┬───────────────────────────────────┘  │
│  ┌──────────────┬──────────┴───────────┬──────────────────────┐    │
│  │  Http Layer  │    Domain Layer      │    Infra Layer       │    │
│  │  (Handlers)  │    (Services)        │    (Adapters)        │    │
│  │  AuthHandler │   AuthService        │   UserRepo (MySQL)   │    │
│  │  ProblemH..  │   ProblemService     │   ProblemRepo        │    │
│  │  SubmitH..   │   SubmissionService  │   SubmissionRepo     │    │
│  │  AdminH..    │   JudgeDispatcher    │   DockerClient       │    │
│  │              │                      │   JwtService         │    │
│  │              │                      │   PasswordHasher     │    │
│  └──────────────┴──────────────────────┴──────────────────────┘    │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                  Judge Worker Pool (4 threads)                │  │
│  │  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │  │
│  │  │ Worker1 │  │ Worker2 │  │ Worker3 │  │ Worker4 │          │  │
│  │  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘          │  │
│  └───────┼────────────┼────────────┼────────────┼────────────────┘  │
└──────────┼────────────┼────────────┼────────────┼───────────────────┘
           │  Docker Engine API (HTTP/UNIX socket)
           ▼
┌────────────────────────────────────────────────────────────────────┐
│  judge-cpp  │  judge-java  │  judge-python  │  judge-go  │
│  (容器)     │  (容器)       │  (容器)         │  (容器)     │ (容器)   │
│  -O2 + 沙箱 │  -Xmx256M 沙箱│  -X... + 沙箱    │  +沙箱     │ +沙箱    │
└────────────────────────────────────────────────────────────────────┘
                                  ▲
                                  │ 卷载 (代码 + 测试点)
                                  │ /judge/host  <->  /judge/work
┌─────────────────────────────────┴──────────────────────────────────┐
│                  MySQL 8.0 Container                              │
│  tables: users / problems / testcases / submissions /             │
│          submission_cases                                         │
└────────────────────────────────────────────────────────────────────┘
```

### 3.2 后端架构

#### 3.2.1 目录结构
```
backend/
├── CMakeLists.txt
├── Dockerfile
├── cmake/                       # FetchContent 声明
│   └── deps.cmake
├── include/                     # 公开头
│   ├── http/
│   │   ├── server.hpp
│   │   ├── router.hpp
│   │   └── middleware/
│   │       ├── auth.hpp
│   │       ├── error.hpp
│   │       └── access_log.hpp
│   ├── domain/
│   │   ├── auth_service.hpp
│   │   ├── problem_service.hpp
│   │   ├── submission_service.hpp
│   │   └── judge_dispatcher.hpp
│   ├── infra/
│   │   ├── mysql_client.hpp
│   │   ├── user_repo.hpp
│   │   ├── problem_repo.hpp
│   │   ├── submission_repo.hpp
│   │   ├── docker_client.hpp
│   │   ├── jwt_service.hpp
│   │   └── password_hasher.hpp
│   └── common/
│       ├── error_code.hpp
│       ├── response.hpp
│       └── config.hpp
├── src/                         # 实现 (与 include 镜像)
│   ├── main.cpp
│   ├── http/
│   ├── domain/
│   ├── infra/
│   └── common/
├── sql/
│   ├── 001_init.sql             # 建表
│   └── 002_seed.sql             # 预置 8 个标签
├── tests/                       # 单元测试（GoogleTest）
└── config/
    └── default.json             # 端口、数据库、判题线程数等
```

#### 3.2.2 关键类职责

| 类 | 职责 |
|---|---|
| `HttpServer` | 包装 cpp-httplib，绑定监听、注册路由、挂中间件 |
| `Router` | 注册 HTTP method + path → handler，支持 path 参数 |
| `AuthMiddleware` | 解析 Authorization Header、验证 JWT、向 handler 注入 `user_id` / `is_admin` |
| `ErrorMiddleware` | 捕获 handler 异常、统一错误码响应 |
| `AccessLogMiddleware` | 记录 access log（方法、路径、状态、耗时、user_id） |
| `AuthService` | 注册、登录、刷新 token、登出 |
| `ProblemService` | 题目 CRUD、列表分页过滤、详情查询 |
| `SubmissionService` | 创建提交、查询状态、查询列表；总分聚合 |
| `JudgeDispatcher` | 后台线程池、轮询 MySQL 任务表、调用 DockerClient |
| `MysqlClient` | 封装 libmysqlclient C API，连接池（≥ 8 连接） |
| `DockerClient` | 通过 HTTP 调用 Docker Engine API（libcurl）：创建容器、启动、等待、读取日志、删除 |
| `JwtService` | 颁发 / 验证 Access / Refresh Token（HS256） |
| `PasswordHasher` | Argon2id 哈希 / 校验 |

#### 3.2.3 配置项（config/default.json）
```json
{
  "server": { "host": "0.0.0.0", "port": 8080 },
  "mysql": { "host": "mysql", "port": 3306, "user": "oj", "password": "oj", "database": "oj" },
  "jwt": { "secret": "change-me-in-prod", "access_ttl_sec": 7200, "refresh_ttl_sec": 604800 },
  "judge": { "worker_count": 4, "poll_interval_ms": 500, "default_time_limit_ms": 2000, "default_memory_limit_mb": 256, "default_output_limit_mb": 64, "code_max_bytes": 65536, "problem_md_max_bytes": 65536 },
  "log": { "level": "info", "dir": "/var/log/oj", "max_size_mb": 100, "max_files": 10 },
  "docker": { "host": "unix:///var/run/docker.sock" }
}
```

### 3.3 前端架构

#### 3.3.1 目录结构
```
frontend/
├── index.html                   # 单 HTML 入口
├── css/
│   ├── base.css                 # 重置 + 变量 (深色主题)
│   ├── layout.css               # 布局
│   └── components.css           # 按钮 / 卡片 / 表格 / 表单
├── js/
│   ├── main.js                  # 入口：启动路由
│   ├── router.js                # History API 路由
│   ├── api/
│   │   ├── client.js            # fetch 封装、自动刷新 token
│   │   ├── auth.js
│   │   ├── problems.js
│   │   └── submissions.js
│   ├── views/                   # 视图函数 (返回 DOM)
│   │   ├── home.js
│   │   ├── login.js
│   │   ├── register.js
│   │   ├── problem-list.js
│   │   ├── problem-detail.js
│   │   ├── submission-list.js
│   │   ├── submission-detail.js
│   │   ├── admin-problem-list.js
│   │   └── admin-problem-edit.js
│   ├── components/              # 复用 UI
│   │   ├── header.js
│   │   ├── footer.js
│   │   ├── monaco-loader.js     # CDN 加载 + 多语言
│   │   ├── markdown-renderer.js
│   │   └── toast.js
│   ├── store/
│   │   └── state.js             # 简单响应式状态
│   └── utils/
│       ├── dom.js
│       ├── format.js
│       └── poller.js            # 通用轮询工具
└── assets/                      # 静态资源
```

#### 3.3.2 路由表
| 路径 | 视图 | 权限 |
|---|---|---|
| `/` | 首页（功能介绍） | 游客 |
| `/login` | 登录 | 游客 |
| `/register` | 注册 | 游客 |
| `/problems` | 题目列表 | 全部 |
| `/problems/:id` | 题目详情 | 全部 |
| `/submissions` | 个人提交列表 | 登录 |
| `/submissions/:id` | 提交详情 | 全部（仅 AC 公开） |
| `/admin/problems` | 后台题目管理 | admin |
| `/admin/problems/new` | 新建题目 | admin |
| `/admin/problems/:id/edit` | 编辑题目 | admin |
| `/profile` | 个人资料 | 登录 |

未匹配路径 → 404 视图。

#### 3.3.3 状态管理
- 不引入框架；用 `store/state.js` 提供 `createSignal` / `createComputed` 风格的极简响应式
- 全局状态：当前用户、Token、主题
- 局部状态：视图组件内部变量
- Token 存储：Access Token 存 `localStorage`、Refresh Token 由后端 Set-Cookie

#### 3.3.4 第三方库（CDN）
- Monaco Editor：从 `https://cdn.jsdelivr.net/npm/monaco-editor@0.45.0/min/vs` 加载
- markdown-it：从 CDN 加载用于客户端预览
- 无其他依赖

#### 3.3.5 前端页面规格

##### A. 全局共享组件

| 组件 | 文件 | 作用 |
|---|---|---|
| `Header` | `components/header.js` | 顶部导航：Logo / 题目 / 提交 / 后台(admin) / 用户菜单 |
| `Footer` | `components/footer.js` | 底部：版权 + 部署版本 |
| `Toast` | `components/toast.js` | 全局成功/错误提示（右上角浮窗，3s 自动消失）|
| `Loading` | `utils/dom.js#loading` | 居中旋转圈 + "加载中..." 文案 |
| `Empty` | `utils/dom.js#empty` | 空数据占位（图标 + 提示语 + 可选 CTA）|
| `Pagination` | `utils/dom.js#pagination` | 通用分页器（首页/上一页/页码/下一页/末页/跳转）|
| `DifficultyBadge` | `utils/dom.js#difficulty` | 难度徽章：易(绿) / 中(橙) / 难(红) |
| `TagChip` | `utils/dom.js#tag` | 标签胶囊（点击触发过滤）|
| `StatusBadge` | `utils/dom.js#status` | 8 态结果徽章（颜色映射见 J）|

##### B. 全局约定
- 路由未匹配 → 渲染 `views/not-found.js`（404 页 + 回首页按钮）
- 未登录访问受保护页 → 跳 `/login?redirect=<原路径>`，登录成功后回跳
- 普通用户访问 `/admin/*` → 跳 `/` 并 Toast "无权限"
- 全局网络错误（fetch reject）→ Toast "网络错误，请重试"
- HTTP 401 → 触发静默刷新，失败则清 Token 跳登录
- HTTP 5xx → 顶部红色 Banner "服务异常" + 详情可点开

##### C. 页面清单（10 个主页面 + 1 个 404）

| # | 路径 | 视图 | 权限 |
|---|---|---|---|
| 1 | `/` | 首页 | 游客 |
| 2 | `/login` | 登录 | 游客 |
| 3 | `/register` | 注册 | 游客 |
| 4 | `/problems` | 题目列表 | 全部 |
| 5 | `/problems/:id` | 题目详情 | 全部 |
| 6 | `/submissions` | 个人提交列表 | 登录 |
| 7 | `/submissions/:id` | 提交详情 | 全部（仅 AC 公开）|
| 8 | `/admin/problems` | 后台题目管理 | admin |
| 9 | `/admin/problems/new` | 新建题目 | admin |
| 10 | `/admin/problems/:id/edit` | 编辑题目 | admin |
| 11 | `/profile` | 个人资料 | 登录 |
| — | `*` | 404 | 全部 |

##### D. 页面 1：首页 `/`

**目的**：项目介绍 + 引导注册/进入题库

**布局**：
```
┌─────────────────────────────────────┐
│ Header                              │
├─────────────────────────────────────┤
│  Hero 区                            │
│   - 大标题 "OnlineJudge"            │
│   - 副标题 "5 语言 / Docker 沙箱"   │
│   - CTA 按钮：[开始刷题] [注册]     │
├─────────────────────────────────────┤
│  特性卡片 3 列：                    │
│   - 5 种语言  /  自动判分  /  安全  │
├─────────────────────────────────────┤
│  数据统计（登录后显示本人数据）：   │
│   - 已通过 / 总提交 / 尝试中题数   │
├─────────────────────────────────────┤
│ Footer                              │
└─────────────────────────────────────┘
```

**数据**：游客只展示静态介绍；登录后调 `/api/auth/me` + 个人统计接口（合并到 `/api/auth/me`）

**状态**：正常态（无 loading/empty 概念）

##### E. 页面 2：登录 `/login`

**布局**：居中卡片（宽 400px）
```
┌────────────────────────┐
│  登录                  │
│  用户名: [_________]   │
│  密码:   [_________]   │
│  [    登 录    ]       │
│  还没有账号? 立即注册  │
└────────────────────────┘
```

**交互**：
- 提交时按钮 disabled + "登录中..."
- 成功 → 跳 `?redirect=` 参数或 `/`
- 失败 → Toast 显示 `message`
- 用户名 / 密码前端校验（非空、密码 ≥ 8）

##### F. 页面 3：注册 `/register`

**布局**：同登录卡片，多两字段
- 字段：用户名、邮箱、密码、确认密码
- 用户名：3–20 字符、仅 `[a-zA-Z0-9_]`、实时校验唯一性（失焦时调 `/api/auth/check-username`）
- 密码：≥ 8 字符、含字母+数字（前端正则）
- 确认密码：必须等于密码
- 提交按钮 disabled 直到所有校验通过

**成功后**：弹 Toast "注册成功，已自动登录" + 跳 `/`

##### G. 页面 4：题目列表 `/problems`

**布局**：
```
┌────────────────────────────────────────┐
│ Header                                 │
├────────────────────────────────────────┤
│  [搜索框_________] [难度▼] [标签▼×3]   │
│                              [排序▼]   │
├────────────────────────────────────────┤
│  ┌──────┐  ┌──────┐  ┌──────┐         │
│  │ 卡片 │  │ 卡片 │  │ 卡片 │  ...     │
│  └──────┘  └──────┘  └──────┘         │
├────────────────────────────────────────┤
│  分页器  共 N 条 [上一页 1 2 3 下一页]│
└────────────────────────────────────────┘
```

**卡片内容**：标题 / 难度徽章 / 标签 chip（最多 3 个 + "..."）/ 通过率 / 箭头

**交互**：
- 过滤条件变化 → 立即重查（debounce 300ms）
- 卡片点击 → 跳 `/problems/:id`
- admin 视角：卡片右下角显示"已通过 X / Y"（仅 admin 可见）

**状态**：
- loading：8 个骨架卡片
- empty：`Empty` 组件 + "没有符合条件的题目" + "清除过滤" 按钮
- error：顶部红色 Banner + 重试按钮

**API**：`GET /api/problems?page=1&size=20&difficulty=&tag=&sort=&q=`

##### H. 页面 5：题目详情 `/problems/:id`

**布局**（两栏，左 50% 右 50%，窄屏堆叠）：
```
┌──────────────────┬──────────────────┐
│  左栏：题面      │  右栏：编辑器    │
│  ┌────────────┐  │  ┌────────────┐  │
│  │ 标题        │  │  │ 语言▼ C++  │  │
│  │ 难度 标签   │  │  ├────────────┤  │
│  │ 渲染题面    │  │  │ Monaco     │  │
│  │ (Markdown)  │  │  │ Editor     │  │
│  │ 样例1: ...  │  │  │            │  │
│  │ 样例2: ...  │  │  │            │  │
│  │ 时空限制    │  │  └────────────┘  │
│  └────────────┘  │  [重置] [提交]    │
│                  └──────────────────┘
└──────────────────┴──────────────────┘
```

**关键交互**：
- 进入页面：自动从 localStorage 恢复上次未提交的代码（key=`draft:problem_<id>:<lang>`）
- 编辑器内容变化时：debounce 500ms 写回 localStorage
- 语言切换时：保存当前代码到 `draft:problem_<id>:<旧lang>`，从 `draft:problem_<id>:<新lang>` 恢复
- 点击"提交"：弹模态确认（"确认提交 X 语言？"），确认后 POST `/api/submissions`
- 提交成功后：跳 `/submissions/<新id>`，并 Toast "已提交，判题中..."

**状态**：
- 题面 loading：左栏骨架
- 题面 404：渲染空状态 + 回列表按钮
- 提交按钮 loading 中：禁用 + "提交中..."
- Monaco 加载失败：右栏降级为 `<textarea>`，并提示 "高级编辑器加载失败，已切换到简单模式"

##### I. 页面 6：个人提交列表 `/submissions`

**布局**：表格 + 过滤
```
┌──────────────────────────────────────────────────────┐
│ Header                                               │
├──────────────────────────────────────────────────────┤
│ [题目▼] [语言▼] [状态▼]              共 N 条         │
├──────────────────────────────────────────────────────┤
│ ID │ 题目 │ 语言 │ 状态 │ 分数 │ 耗时 │ 内存 │ 时间 │ 操作 │
│ 123│ 两数之和│ C++│ [AC] │ 100 │ 15ms │ 4MB │ ... │ 查看 │
│ 122│ ...                                            │
├──────────────────────────────────────────────────────┤
│ 分页器                                               │
└──────────────────────────────────────────────────────┘
```

**关键交互**：
- 表格行点击 → 跳 `/submissions/:id`
- 过滤条件变化 → 立即重查
- 时间列：相对时间（"3 分钟前"）+ hover 显示绝对时间
- 状态徽章：8 色映射（见 J）

**API**：`GET /api/submissions?user=me&page=1&problem_id=&language=&status=`

##### J. 8 态状态徽章颜色映射

| 状态 | 颜色 | 文字 |
|---|---|---|
| AC | 绿色 #4ade80 | 通过 |
| WA | 红色 #f87171 | 答案错误 |
| TLE | 黄色 #facc15 | 超时 |
| MLE | 橙色 #fb923c | 超内存 |
| OLE | 紫色 #c084fc | 输出超限 |
| RE | 红色 #ef4444 | 运行错误 |
| CE | 灰色 #9ca3af | 编译错误 |
| SE | 灰色 #6b7280 | 系统错误 |

##### K. 页面 7：提交详情 `/submissions/:id`

**布局**：
```
┌──────────────────────────────────────────────────┐
│ Header                                           │
├──────────────────────────────────────────────────┤
│  提交 #123  [WA]  总分 60/100                   │
│  用户: alice  语言: C++  耗时: 1500ms 内存: 12MB │
│  提交时间: 2026-04-23 10:00:00  判完: 10:00:15   │
├──────────────────────────────────────────────────┤
│  [源代码]  [测试点]                              │
│  ┌────────────────────────────────────────────┐ │
│  │  Monaco (只读)                              │ │
│  │  #include<bits/stdc++.h>                    │ │
│  │  ...                                        │ │
│  └────────────────────────────────────────────┘ │
│  或：测试点表格                                  │
│  # │ 状态 │ 耗时 │ 内存 │ 分数 │ 详情            │
│  1 │ [AC] │ 10ms │ 4MB  │ 30   │ [查看] (样例)  │
│  2 │ [AC] │ 12ms │ 4MB  │ 30   │ (隐藏)         │
│  3 │ [WA] │ 50ms │ 8MB  │ 0    │ [查看] (隐藏)  │
└──────────────────────────────────────────────────┘
```

**关键交互**：
- Tab 切换：源代码 / 测试点
- 错点 "查看" → 弹出 diff 模态框
  - 样例点：左 input / 中 expected / 右 user_output
  - 隐藏点：仅显示"为保护题目，不展示隐藏点详情"
- 若 `result=CE`：源代码区下方展示 `compile_output`（pre 格式保留换行）
- 若 `result=SE`：展示 `judge_message`
- 公开访问：仅当 `result=AC` 或登录后是本人时可看

**状态**：
- 加载中：骨架
- 403：Toast "无权查看" + 回上一页
- 404：Toast "提交不存在"

**API**：`GET /api/submissions/:id`

##### L. 页面 8：后台题目管理 `/admin/problems`

**布局**：表格（与提交列表类似，但行内带操作）
```
ID │ 标题 │ 难度 │ 标签 │ 状态 │ 通过率 │ 创建者 │ 创建时间 │ 操作
 1 │ 两数之和 │ 易 │ 数组 │ 已发布 │ 75% │ alice │ 2026-04-01 │ [编辑][上下架][删除]
```

**关键交互**：
- 顶部 [新建题目] 按钮（蓝）
- "上下架"切换 → PATCH `/api/admin/problems/:id/publish` + 立即刷新
- "删除" → 弹确认模态 → DELETE → 立即刷新
- 状态列：`已发布`(绿) / `草稿`(灰)
- 支持过滤：按状态、按创建者、按标题搜索

**API**：`GET /api/admin/problems?page=1&is_published=&q=`

##### M. 页面 9/10：新建/编辑题目 `/admin/problems/new`、`/admin/problems/:id/edit`

**布局**（两栏，左 60% 题面 + 右 40% 元数据）：
```
┌──────────────────────────────────────────────┐
│  ← 返回列表    新建题目 / 编辑题目 #5         │
├──────────────────────────┬───────────────────┤
│  左栏：题面编辑器        │  右栏：元数据      │
│  ┌────────┬────────────┐│  标题: ___________ │
│  │ 编辑   │ 预览       ││  难度: ◯易 ◯中 ◯难│
│  │ Monaco │ Markdown   ││  标签: ☑数组 ...   │
│  │ MD模式 │ 渲染       ││  时限: [2] 秒      │
│  │        │            ││  内存: [256] MB    │
│  │        │            ││  输出: [64] MB     │
│  │        │            ││  ☑ 立即发布        │
│  └────────┴────────────┘│                    │
│                          │  [保存草稿] [发布] │
├──────────────────────────┴───────────────────┤
│  测试点（拖拽排序行）                         │
│  # │ 输入 │ 预期输出 │ 样例 │ 分值 │ 删除     │
│  1 │ [...][...][☐][30] [×]                   │
│  2 │ [...][...][☐][30] [×]                   │
│  [+ 添加测试点]                               │
│  总分: 60 / 100  ⚠ 分数之和必须等于 100       │
└──────────────────────────────────────────────┘
```

**关键交互**：
- 左栏 Tab 切换：编辑 / 预览（实时同步）
- 标题必填，< 100 字符
- 难度/标签必选
- 测试点：动态增删、score 必填且 ≥ 0、"添加"按钮置于末尾
- 总分实时计算；≠ 100 时 [保存]/[发布] 按钮 disabled + 红色提示
- "保存草稿"= 保存但 `is_published=0`；"发布"= 保存且 `is_published=1`
- 离开页面前若未保存 → `beforeunload` 弹原生确认

**API**：
- 新建：`POST /api/admin/problems` (body 含 content_md + cases[])
- 编辑：`PUT /api/admin/problems/:id` (全量)
- 进入编辑：`GET /api/admin/problems/:id/edit-data` (含完整测试点)

##### N. 页面 11：个人资料 `/profile`

**布局**：简单资料卡
```
┌────────────────────────────────────┐
│  头像: [空]                        │
│  用户名: alice                     │
│  邮箱: alice@example.com           │
│  角色: 管理员 (徽章)                │
│  注册时间: 2026-04-23 10:00:00     │
│  ──────────────────────────────    │
│  数据统计:                         │
│   - 已通过题目: 5 / 总 8           │
│   - 提交总数: 42                   │
│   - 通过率: 65%                    │
│  ──────────────────────────────    │
│  [修改密码]  [退出登录]            │
└────────────────────────────────────┘
```

**关键交互**：
- "修改密码"：弹模态（输入旧密码 + 新密码 + 确认），提交 PATCH `/api/auth/password`
- "退出登录"：确认 → POST `/api/auth/logout` → 清 localStorage → 跳 `/`

**API**：`GET /api/auth/me`（含统计字段扩展）

##### O. 404 页面 `*`

**布局**：居中
- 大字 "404"
- 副标题 "页面不存在"
- [返回首页] 按钮

### 3.4 沙箱安全策略（所有判题容器统一）
- `--network none`：禁止任何网络访问
- `--cap-drop ALL`：移除所有 Linux Capabilities
- `--security-opt no-new-privileges`：禁止提权
- `--read-only`：根文件系统只读
- `--tmpfs /tmp:size=64m,noexec,nosuid`：唯一可写 tmpfs，禁止执行
- `--memory=256m --memory-swap=256m`：硬性内存限制（按题覆写）
- `--cpuset-cpus=0` + `--cpu-quota=200000`：CPU 限制（按题覆写）
- `--pids-limit=64`：进程数限制
- 非 root 用户运行（容器内建 `judge` 用户 UID 1000）
- 代码与测试点通过卷载注入，运行结束自动删除容器（`--rm`）

---

## 4. 数据模型

### 4.1 ER 图

```
┌────────┐         ┌────────────┐         ┌──────────────┐
│  users │────┐    │  problems  │────┐    │  testcases   │
└────────┘    │    └────────────┘    │    └──────────────┘
   │          │         │           │           │
   │          │         │           │           │
   ▼          │         ▼           │           │
┌────────────────────┐  ┌────────────────────┐
│  submissions       │  │  (testcases.problem_id) │
└────────────────────┘  └────────────────────┘
   │
   │
   ▼
┌──────────────────┐
│ submission_cases │
└──────────────────┘
```

### 4.2 表结构

#### `users`
| 字段 | 类型 | 约束 |
|---|---|---|
| id | BIGINT | PK, AUTO_INCREMENT |
| username | VARCHAR(20) | UNIQUE, NOT NULL |
| email | VARCHAR(100) | UNIQUE, NOT NULL |
| password_hash | VARCHAR(255) | NOT NULL |
| is_admin | TINYINT(1) | DEFAULT 0 |
| created_at | DATETIME | DEFAULT CURRENT_TIMESTAMP |

#### `problems`
| 字段 | 类型 | 约束 |
|---|---|---|
| id | BIGINT | PK |
| title | VARCHAR(100) | NOT NULL |
| content_md | MEDIUMTEXT | NOT NULL |
| difficulty | ENUM('easy','medium','hard') | NOT NULL |
| time_limit_ms | INT | DEFAULT 2000 |
| memory_limit_mb | INT | DEFAULT 256 |
| output_limit_mb | INT | DEFAULT 64 |
| is_published | TINYINT(1) | DEFAULT 0 |
| created_by | BIGINT | FK → users.id |
| created_at | DATETIME | |

#### `problem_tags`（多对多）
| 字段 | 类型 |
|---|---|
| problem_id | BIGINT, FK |
| tag_id | INT, FK |

#### `tags`（预置 8 个，不开放后台管理）
| 字段 | 类型 |
|---|---|
| id | INT, PK |
| name | VARCHAR(20) |
| slug | VARCHAR(20) |

预置数据：`数组`/`数组`, `字符串`/`string`, `链表`/`linked-list`, `栈/队列`/`stack-queue`, `树`/`tree`, `图`/`graph`, `动态规划`/`dp`, `贪心`/`greedy`

#### `testcases`
| 字段 | 类型 | 约束 |
|---|---|---|
| id | BIGINT | PK |
| problem_id | BIGINT | FK → problems.id (CASCADE) |
| case_index | INT | NOT NULL |
| input | LONGTEXT | NOT NULL |
| expected_output | LONGTEXT | NOT NULL |
| is_sample | TINYINT(1) | DEFAULT 0 |
| score | INT | NOT NULL |
| UNIQUE | (problem_id, case_index) |

#### `submissions`
| 字段 | 类型 | 约束 |
|---|---|---|
| id | BIGINT | PK |
| user_id | BIGINT | FK |
| problem_id | BIGINT | FK |
| language | ENUM('c','cpp','java','python','go') | |
| code | MEDIUMTEXT | |
| status | ENUM('queued','compiling','running','finished') | |
| result | ENUM('AC','WA','TLE','MLE','OLE','RE','CE','SE') | NULLABLE |
| total_score | INT | DEFAULT 0 |
| time_used_ms | INT | DEFAULT 0 |
| memory_used_kb | INT | DEFAULT 0 |
| compile_output | MEDIUMTEXT | |
| judge_message | VARCHAR(500) | |
| created_at | DATETIME | |
| finished_at | DATETIME | NULLABLE |
| INDEX | (status, created_at), (user_id, created_at) |

#### `submission_cases`
| 字段 | 类型 | 约束 |
|---|---|---|
| id | BIGINT | PK |
| submission_id | BIGINT | FK (CASCADE) |
| case_index | INT | |
| status | ENUM('AC','WA','TLE','MLE','OLE','RE') | |
| time_used_ms | INT | |
| memory_used_kb | INT | |
| score | INT | |
| is_sample | TINYINT(1) | |
| user_output | LONGTEXT | **仅 is_sample=1 存储** |

### 4.3 数据库初始化
- 启动时由后端 `db_init` 命令读取 `sql/001_init.sql` + `sql/002_seed.sql`
- 幂等：使用 `CREATE TABLE IF NOT EXISTS` / `INSERT IGNORE`

---

## 5. 接口契约

### 5.1 通用规范

- 全部 JSON 请求/响应（`Content-Type: application/json; charset=utf-8`）
- 统一响应格式：
```json
{ "code": 0, "message": "ok", "data": <T> }
```
- 错误码：
| code | 含义 | HTTP |
|---|---|---|
| 0 | 成功 | 200/201 |
| 1001 | 参数错误 | 400 |
| 1002 | 未认证 | 401 |
| 1003 | 无权限 | 403 |
| 1004 | 资源不存在 | 404 |
| 1005 | 资源冲突（如用户名已存在） | 409 |
| 1006 | 资源超限（提交过频/代码过长） | 413 |
| 1007 | 服务器内部错误 | 500 |
| 1008 | 系统错误（判题机） | 500 |
- Refresh Token 通过 HttpOnly Cookie 下发：`Set-Cookie: refresh_token=...; HttpOnly; SameSite=Lax; Path=/api/auth`

### 5.2 接口列表

#### 5.2.1 认证
| Method | Path | 鉴权 | Body / Query | 响应 data |
|---|---|---|---|---|
| POST | `/api/auth/register` | 否 | `{username, email, password}` | `{user_id, access_token, is_admin}` |
| POST | `/api/auth/login` | 否 | `{username, password}` | `{user_id, access_token, is_admin}` |
| POST | `/api/auth/refresh` | Cookie | — | `{access_token}` |
| POST | `/api/auth/logout` | Cookie | — | `null` |
| GET | `/api/auth/me` | Bearer | — | `{user_id, username, email, is_admin}` |

#### 5.2.2 题目
| Method | Path | 鉴权 | 备注 |
|---|---|---|---|
| GET | `/api/problems` | 否 | `?page=1&size=20&difficulty=easy&tag=dp&sort=created_desc` |
| GET | `/api/problems/{id}` | 否 | 返回题面 + 样例点 |
| GET | `/api/tags` | 否 | 预置 8 个标签 |

#### 5.2.3 提交
| Method | Path | 鉴权 | 备注 |
|---|---|---|---|
| POST | `/api/submissions` | Bearer | body: `{problem_id, language, code}` → `{submission_id}` |
| GET | `/api/submissions/{id}` | Bearer | 含逐点状态（按 2.3.2 规则） |
| GET | `/api/submissions` | Bearer | `?user=me&page=1&problem_id=&language=&status=` |
| GET | `/api/submissions/public` | 否 | 仅 `result=AC` 公开列表 |

#### 5.2.4 后台管理（仅 admin）
| Method | Path | 备注 |
|---|---|---|
| GET | `/api/admin/problems` | 含未发布 |
| POST | `/api/admin/problems` | body: 完整题目 + 测试点数组 |
| PUT | `/api/admin/problems/{id}` | 全量更新 |
| DELETE | `/api/admin/problems/{id}` | 软删除（is_published=0） |
| PATCH | `/api/admin/problems/{id}/publish` | 上下架 |
| PATCH | `/api/admin/users/{id}/role` | 设置 admin |

### 5.3 响应示例

`GET /api/submissions/123`：
```json
{
  "code": 0,
  "message": "ok",
  "data": {
    "id": 123,
    "problem_id": 1,
    "user_id": 5,
    "username": "alice",
    "language": "cpp",
    "code": "#include<bits/stdc++.h>\n...",
    "status": "finished",
    "result": "WA",
    "total_score": 60,
    "time_used_ms": 1500,
    "memory_used_kb": 12000,
    "compile_output": "",
    "judge_message": "",
    "created_at": "2026-04-23T10:00:00Z",
    "finished_at": "2026-04-23T10:00:15Z",
    "cases": [
      {"case_index": 1, "status": "AC", "time_used_ms": 10, "memory_used_kb": 4096, "score": 30, "is_sample": true,  "user_output": "1 2\n",  "expected_output": "1 2\n"},
      {"case_index": 2, "status": "AC", "time_used_ms": 12, "memory_used_kb": 4096, "score": 30, "is_sample": false, "user_output": null, "expected_output": null},
      {"case_index": 3, "status": "WA", "time_used_ms": 50, "memory_used_kb": 8192, "score": 0,  "is_sample": false, "user_output": null, "expected_output": null}
    ]
  }
}
```

---

## 6. 判题子系统

### 6.1 容器协议（run.sh + 卷载 + judge 二进制）

每个判题镜像（`judge-cpp` / `judge-java` / `judge-python` / `judge-go`）内部统一：
- 预装该语言编译器 / 运行时
- 预装一个 C++ 写的小型 `judge` 工具（约 5 MB，静态编译）
- 预装 `entrypoint.sh`

**主机端准备**：
```
/tmp/oj/<submission_id>/
├── src/                     # 源代码（main.cpp / Main.java / main.py / main.go）
├── testcases/
│   ├── 1.in / 1.out
│   ├── 2.in / 2.out
│   └── ...
├── meta.json                # { time_limit_ms, memory_limit_mb, output_limit_mb, language }
└── result/                  # 输出目录（卷载）
    ├── compile.log
    ├── per_case.json        # judge 工具写入
    └── summary.json
```

**容器内 `entrypoint.sh`**：
```bash
#!/bin/bash
set -e
cd /judge/work
/judge/bin/judge --meta meta.json --src src/ --tests testcases/ --out result/
```

**`judge` 工具的逻辑**（按语言分支）：
1. 调用对应编译器：`g++ -O2 -std=c++17 -o bin src/main.cpp`（其他语言类似）
   - 编译失败 → 写 `compile.log`，退出 10
2. 对每个测试点：
   a. 用 `timeout + rlimit` 启动 `bin`（或 `python3 src/main.py`）
   b. 监控时间（`getrusage`/子进程 wall-clock）、内存（周期采样 `/proc/<pid>/status`）
   c. 写出文件比对（`diff -b`、忽略末尾空白）
   d. 写入 `per_case.json` 追加一行
3. 退出 0（成功执行完所有点）

**DockerClient 的工作流**（C++ 端）：
1. `POST /containers/create`：`image=judge-cpp, Cmd=["/judge/bin/entrypoint.sh"], HostConfig=沙箱参数, Mounts=卷载 /tmp/oj/<id>`
2. `POST /containers/{id}/start`
3. `POST /containers/{id}/wait`（带超时 = 总时间限制 + 30s 缓冲）
4. `GET /containers/{id}/logs?stdout=1&stderr=1`（仅 CE 时记录到 compile_output）
5. `DELETE /containers/{id}?force=true`（或依赖 `--rm`）
6. 读取 `result/summary.json` 解析为 `submission_cases` 行

### 6.2 5 种语言的编译/运行命令模板

| 语言 | 编译 | 运行 |
|---|---|---|
| C | `gcc -O2 -std=c11 -o bin src/main.c -lm` | `./bin` |
| C++ | `g++ -O2 -std=c++17 -o bin src/main.cpp` | `./bin` |
| Java | `javac -d bin src/Main.java` | `java -Xss64m -Xmx<题目内存>M Main` |
| Python | — | `python3 src/main.py` |
| Go | `go build -o bin src/main.go` | `./bin` |

### 6.3 镜像清单
- `judge-cpp:1.0`：基于 `gcc:13-bookworm`，含 `g++/gcc/judge/entrypoint.sh`
- `judge-java:1.0`：基于 `eclipse-temurin:21-jdk-jammy`
- `judge-python:1.0`：基于 `python:3.12-slim`
- `judge-go:1.0`：基于 `golang:1.22-alpine`
- 每个镜像大小 < 500 MB；判题机在主机编译时 `docker build`，通过 `docker-compose build` 一键构建

### 6.4 沙箱安全（与 3.4 一致，此处为容器启动时的 Docker API 形式）
```json
{
  "NetworkMode": "none",
  "CapDrop": ["ALL"],
  "SecurityOpt": ["no-new-privileges"],
  "ReadonlyRootfs": true,
  "Tmpfs": { "/tmp": "size=64m,noexec,nosuid,nodev" },
  "Memory": 268435456,
  "MemorySwap": 268435456,
  "CpuQuota": 200000,
  "CpuPeriod": 100000,
  "PidsLimit": 64,
  "User": "judge",
  "AutoRemove": true
}
```
（注：题目的 `time_limit_ms` 由 `judge` 工具内部 `setrlimit` + `timeout` 控制，非 Docker 层。）

---

## 7. 部署

### 7.1 仓库目录总览

> **本章是仓库文件树的唯一权威参考**。各子目录内部结构分别在 §3.2.1（backend）、§3.3.1（frontend）、§6.3（judge-images）有细节展开，此处给出**全貌**与**文件用途注解**。

```
onlinejudge/
│
├── SPEC.md                          ← 本文档，唯一需求基线
├── README.md                        ← 快速开始 / 本地开发 / 部署
├── docker-compose.yml               ← 一键启动所有服务
├── .gitignore                       ← 忽略 build/、.vscode/、*.o 等
├── .dockerignore                    ← 排除 .git、build/、tests/ 等入镜像
│
├── backend/                         ◀── §3.2.1 C++20 后端
│   ├── CMakeLists.txt               ← 顶层构建脚本
│   ├── Dockerfile                   ← backend 镜像构建
│   │
│   ├── cmake/
│   │   └── deps.cmake               ← FetchContent 拉 cpp-httplib/jwt-cpp/libcurl/nlohmann/json
│   │
│   ├── include/                     ← 公开头（4 层目录）
│   │   ├── common/                  ← 跨层公共：error_code / response / config
│   │   ├── http/                    ← HTTP 层：server / router / middleware/{auth,error,access_log}
│   │   ├── domain/                  ← 业务层：auth_service / problem_service / submission_service / judge_dispatcher
│   │   └── infra/                   ← 基础设施：mysql_client / user_repo / problem_repo / submission_repo / docker_client / jwt_service / password_hasher
│   │
│   ├── src/                         ← 实现（与 include 目录镜像）
│   │   ├── main.cpp                 ← 入口：装配服务、启动 HTTP、启动 JudgeDispatcher
│   │   ├── common/                  ← error_code.cpp / response.cpp / config.cpp
│   │   ├── http/                    ← server.cpp / router.cpp / middleware/*.cpp
│   │   ├── domain/                  ← 业务服务实现
│   │   └── infra/                   ← Repo / Client 实现
│   │
│   ├── sql/                         ← 数据库脚本（MySQL 首次启动自动执行 + 后端 db_init 命令读取）
│   │   ├── 001_init.sql             ← 7 张表 DDL
│   │   └── 002_seed.sql             ← 预置 8 个标签
│   │
│   ├── tests/                       ← 单元测试（GoogleTest）
│   │   ├── test_auth.cpp            ← AuthService / JwtService / PasswordHasher
│   │   ├── test_problem.cpp         ← ProblemService / ProblemRepo
│   │   ├── test_submission.cpp      ← SubmissionService
│   │   └── test_judge_dispatcher.cpp← Mock DockerClient 测状态机
│   │
│   └── config/
│       └── default.json             ← 端口/数据库/判题线程数等
│
├── frontend/                        ◀── §3.3.1 原生 HTML/CSS/JS
│   ├── index.html                   ← 唯一 HTML 入口
│   │
│   ├── css/
│   │   ├── base.css                 ← 重置 + CSS 变量（深色主题）
│   │   ├── layout.css               ← 布局：header/main/footer 三段
│   │   └── components.css           ← 按钮/卡片/表格/表单/徽章
│   │
│   ├── js/
│   │   ├── main.js                  ← 启动入口
│   │   ├── router.js                ← History API 路由
│   │   │
│   │   ├── api/                     ← API 客户端（按域拆分）
│   │   │   ├── client.js            ← fetch 封装 + 自动刷新 token
│   │   │   ├── auth.js              ← 5 个 Auth API
│   │   │   ├── problems.js          ← 题目 API
│   │   │   └── submissions.js       ← 提交 API
│   │   │
│   │   ├── views/                   ← 视图（一个文件一个页面）
│   │   │   ├── home.js              ← 1. 首页
│   │   │   ├── login.js             ← 2. 登录
│   │   │   ├── register.js          ← 3. 注册
│   │   │   ├── problem-list.js      ← 4. 题目列表
│   │   │   ├── problem-detail.js    ← 5. 题目详情
│   │   │   ├── submission-list.js   ← 6. 个人提交列表
│   │   │   ├── submission-detail.js ← 7. 提交详情
│   │   │   ├── admin-problem-list.js    ← 8. 后台题目管理
│   │   │   ├── admin-problem-edit.js    ← 9/10. 新建/编辑题目
│   │   │   ├── profile.js           ← 11. 个人资料
│   │   │   └── not-found.js         ← 404 页面
│   │   │
│   │   ├── components/              ← 复用 UI
│   │   │   ├── header.js            ← 顶部导航
│   │   │   ├── footer.js            ← 底部版权
│   │   │   ├── toast.js             ← 全局消息提示
│   │   │   ├── monaco-loader.js     ← CDN 加载 Monaco + 多语言
│   │   │   └── markdown-renderer.js ← markdown-it 包装
│   │   │
│   │   ├── store/
│   │   │   └── state.js             ← 极简响应式 store
│   │   │
│   │   └── utils/                   ← 工具函数
│   │       ├── dom.js               ← createEl / loading / empty / pagination / 徽章
│   │       ├── format.js            ← 时间/字节数/百分比格式化
│   │       └── poller.js            ← 通用轮询
│   │
│   └── assets/                      ← 图片、Logo、favicon 等
│
├── judge-images/                    ◀── §6.3 5 个语言镜像
│   ├── common/                      ← 共享源码（构建进每个镜像）
│   │   ├── judge.cpp               ← 判题工具 C++ 源码（编译/运行/资源监控）
│   │   ├── judge.hpp
│   │   ├── CMakeLists.txt           ← 静态编译 judge 二进制
│   │   └── entrypoint.sh            ← 容器内启动脚本
│   │
│   ├── cpp/
│   │   ├── Dockerfile               ← 基于 gcc:13-bookworm
│   │   └── Makefile                 ← （可选）测试编译
│   │
│   ├── java/
│   │   └── Dockerfile               ← 基于 eclipse-temurin:21-jdk-jammy
│   │
│   ├── python/
│   │   └── Dockerfile               ← 基于 python:3.12-slim
│   │
│   └── go/
│       └── Dockerfile               ← 基于 golang:1.22-alpine
│
├── nginx/                           ← 前端可选反向代理
│   └── nginx.conf                   ← SPA 路由 fallback 到 index.html
│
└── docs/                            ← 附加文档（可选）
    ├── architecture.md              ← 架构详细说明
    ├── api.md                       ← API 完整文档（由 SPEC §5 同步）
    ├── dev-guide.md                 ← 本地开发步骤
    └── phase1-verification.md       ← Phase 1 一键启动验证报告（AC-1/AC-2 实测）
```

**目录用途速查**：

| 目录 | 作用 | 何时创建 |
|---|---|---|
| `backend/` | C++20 后端服务 | M1 |
| `frontend/` | 原生 HTML/CSS/JS 前端 | M1 |
| `judge-images/` | 5 种语言的判题沙箱镜像 | M4 |
| `backend/sql/` | 数据库初始化脚本 (MySQL 首次启动自动执行) | M1 |
| `nginx/` | 反向代理配置（可选） | M1 |

**文件命名约定**：

- 后端 C++：文件名小写+下划线（`user_repo.hpp` / `mysql_client.cpp`）
- 后端类名：蛇形命名（`UserRepo` / `MysqlClient`）
- 前端 ESM：文件名 kebab-case 或小写驼峰（`problem-list.js` / `monaco-loader.js`）
- 数据库：表名小写下划线（`users` / `submission_cases`）
- SQL 脚本：三位数字前缀按依赖顺序（`001_init.sql` / `002_seed.sql`）

### 7.2 docker-compose.yml 服务清单
| 服务 | 镜像 | 端口 | 卷 | 依赖 |
|---|---|---|---|---|
| `mysql` | `mysql:8.0` | 3306 (内部) | `mysql_data:/var/lib/mysql`, `./backend/sql` | — |
| `backend` | `backend:1.0` (本地 build) | 8080 | `./backend`, `docker.sock` | mysql |
| `frontend` (可选) | `nginx:alpine` | 80 (对外) | `./frontend`, `./nginx.conf` | backend |
| 各 judge 镜像 | `judge-*:1.0` (本地 build) | — | — | — |

`backend` 通过 `docker.sock` 卷载访问宿主 Docker daemon，直接用 `DockerClient` 创建一次性容器。
`docker-compose build` 一次性构建所有镜像。

### 7.3 启动
```bash
git clone <repo>
cd onlinejudge
docker compose up -d --build
# 等待 ~30s
open http://localhost
```

### 7.4 端口与网络
- 对外：80（前端，可选），8080（后端直连）
- 内部：MySQL 3306 仅 backend 可见
- 判题容器：`network_mode: none`，与任何网络隔离

---

## 8. TODO 清单

### Phases 1 - 基础骨架
- [x] 仓库初始化、目录结构、CMakeLists、Dockerfile、docker-compose.yml
- [x] MySQL 初始化 SQL（建表 + 8 个标签）
- [x] C++ 后端骨架：HttpServer 启动、Health endpoint、健康检查
- [x] 前端骨架：单页 + History 路由 + 深色主题 base CSS
- [x] Docker Compose 一键启动验证（详见 §9.5 / Phase 1 验收报告 `docs/phase1-verification.md`）

### Phases 2 - 账户系统
- [x] users 表 / Argon2 密码哈希（详见 §9.6 / `docs/phase2-verification.md`）
- [x] `/api/auth/register` 实现首注册为 admin 逻辑（详见 §9.7 / `docs/phase3-verification.md`）
- [x] `/api/auth/login` + JWT 颁发
- [x] `/api/auth/refresh` + Refresh Cookie + 静默刷新
- [x] 前端：登录/注册页 + Token 存储 + API 客户端

### Phases 3 - 题目模块
- [ ] problems / testcases / tags 表 + Repo
- [ ] `GET /api/problems`（分页、过滤、排序）
- [ ] `GET /api/problems/{id}`（含样例点）
- [ ] `GET /api/tags`
- [ ] 前端：题目列表页 + 题目详情页（Markdown 渲染 + Monaco 编辑器）
- [ ] localStorage 草稿自动保存

### Phases 4 - 判题子系统
- [ ] `judge` 工具（C++）：编译 + 逐点运行 + 资源监控
- [ ] 5 个 judge 镜像 Dockerfile + entrypoint.sh
- [ ] DockerClient（C++，libcurl 调用 Engine API）
- [ ] JudgeDispatcher 线程池（4 worker）+ MySQL 任务轮询
- [ ] submission / submission_cases 表
- [ ] `POST /api/submissions` + `GET /api/submissions/{id}`
- [ ] 前端：提交后 2s 轮询 + 状态机可视化

### Phases 5 - 后台管理
- [ ] admin API：CRUD + 上下架
- [ ] 前端：管理后台题目列表 + Monaco Markdown 编辑器 + 客户端预览
- [ ] 测试点动态增删 + 校验总分 = 100

### Phases 6 - 提交历史 + 详情
- [ ] 提交列表（个人 / 公共）
- [ ] 提交详情：Monaco 只读 + 逐点状态表格 + 错点 diff

### Phases 7 - 打磨与验收
- [ ] spdlog 接入 + access log
- [ ] 统一错误中间件
- [ ] 单元测试（GoogleTest）：Auth / Problem / Judge 关键路径
- [ ] README：本地开发 + 部署文档
- [ ] 端到端验证：按 9 章验收清单全过

---

## 9. 验收标准

### 9.1 功能验收（必须全过）

#### 9.1.1 启动与注册
- [ ] AC-1：`docker compose up -d --build` 一次成功，< 5 分钟可用
- [ ] AC-2：访问 `http://localhost` 加载首页，DOM 渲染无 JS 报错
- [ ] AC-3：第一个注册用户自动获得 admin 权限；第二个注册用户为普通用户
- [ ] AC-4：用户名重复、邮箱重复、密码 < 8 字符均返回 1005/1001

#### 9.1.2 题目录入
- [ ] AC-5：admin 在后台创建一道题，题面 Markdown 实时预览正确
- [ ] AC-6：测试点 score 之和不等于 100 时提交按钮禁用
- [ ] AC-7：未发布题目对普通用户不可见

#### 9.1.3 提交流程（5 种语言各跑一遍）
对每种语言 (C/C++/Java/Python/Go)，准备 1 道两数之和题 + 1 道 TLE 题 + 1 道 WA 题：
- [ ] AC-8：AC 题 → result=AC, total_score=100, cases 全 AC
- [ ] AC-9：TLE 题 → result=TLE, 至少一个 case status=TLE
- [ ] AC-10：WA 题 → result=WA, 至少一个 case status=WA
- [ ] AC-11：CE 题（C++ 故意写错语法）→ result=CE, compile_output 含编译器原文
- [ ] AC-12：RE 题（除以 0）→ result=RE

#### 9.1.4 沙箱安全
- [ ] AC-13：提交 `while(1){}` 在 Java / Python / C++ 三种语言下均 TLE 且不挂死 backend
- [ ] AC-14：提交 `fork bomb`（`bash -c ':(){ :|:&};:`）不挂死宿主机
- [ ] AC-15：提交 `curl http://example.com` 无网络访问（容器 network=none），判为 RE 或 TLE
- [ ] AC-16：提交读取宿主机 `/etc/passwd` 的代码（symlink 越狱尝试）失败，容器只读

#### 9.1.5 提交历史
- [ ] AC-17：个人提交列表分页正确
- [ ] AC-18：AC 提交对其他用户可见，非 AC 仅自己可见
- [ ] AC-19：错点详情页展示 user_output（仅 is_sample=1）/expected/diff

#### 9.1.6 并发与稳定性
- [ ] AC-20：连续 50 次提交同一 AC 代码，全部判 AC，无数据库 deadlock
- [ ] AC-21：判题线程池 4 满时，第 5 个提交进入 queued 状态，2s 内被轮询
- [ ] AC-22：MySQL 容器手动 stop 后访问 API 返回 503；恢复后自动可用

### 9.2 性能验收
- [ ] P-1：题目列表接口 P95 < 200ms（1000 题数据集）
- [ ] P-2：提交创建接口 P95 < 200ms
- [ ] P-3：判题端到端（提交到出结果）P95 < 30s（2s 时限的题 + 编译时间）

### 9.3 安全验收
- [ ] S-1：所有 HTTP 响应含 CSP / X-Content-Type-Options / X-Frame-Options 头
- [ ] S-2：密码以 Argon2id 存储，DB 中无可逆值
- [ ] S-3：JWT 过期后访问受保护接口返回 1002
- [ ] S-4：Docker 容器运行时无网络、无特权

### 9.4 可维护性验收
- [ ] M-1：所有 C++ 类遵循 Http/Domain/Infra 分层，依赖单向（Http → Domain ← Infra）
- [ ] M-2：单元测试覆盖率：Auth 关键路径 ≥ 80%
- [ ] M-3：README 含本地开发 5 步指南 + 部署指南 + 常见问题
- [ ] M-4：所有依赖通过 FetchContent 拉取，无 vcpkg / apt 依赖

### 9.5 Phase 1 — Docker Compose 一键启动验证（已通过）
> 触发条件：SPEC §8 TODO「Phases 1 - 基础骨架」中 5 个 checkbox 已完成 4 个，
> 仅剩「Docker Compose 一键启动验证」。本节为该项的**端到端实测报告**，
> 详细命令、原始输出与修复记录见 [`docs/phase1-verification.md`](docs/phase1-verification.md)。

**验证时间**：2026-06-16
**验证环境**：Linux x86_64 / Docker 29.3.1 / Docker Compose v5.1.1

**验收对照表**：

| # | 验收点 | 命令 | 结果 |
|---|---|---|---|
| AC-1a | `docker compose up -d --build` 一次成功 | `docker compose down -v && time docker compose up -d --build` | ✅ ~30 s 完成（warm cache） |
| AC-1b | 全部服务 healthy | `docker compose ps` | ✅ mysql / backend / frontend 均 `Up (healthy)` |
| AC-1c | 启动总耗时 < 5 分钟 | `for i in {1..30}; do ...` | ✅ cold + warm 端到端 < 1 分钟 |
| AC-2a | 浏览器访问 `http://localhost` 返回 HTML | `curl http://127.0.0.1/` | ✅ HTTP 200, 809 B, `text/html; charset=utf-8` |
| AC-2b | 静态资源 CSS/JS 加载 | `curl /css/base.css /js/main.js` | ✅ 全部 HTTP 200 |
| AC-2c | SPA fallback（未知路径回 index.html） | `curl /problems /admin/x/y/z` | ✅ 全部 HTTP 200, 809 B |
| AC-2d | JS 模块导入无 syntax error | `node --check js/*.js` | ✅ main / router / dom / views 全部 OK |
| AC-3a | 数据库 7 张表均已建 | `SHOW TABLES` | ✅ users / tags / problems / problem_tags / testcases / submissions / submission_cases |
| AC-3b | 8 个标签已 seed | `SELECT COUNT(*) FROM tags` | ✅ 8 |
| AC-3b | 标签 utf8mb4 中文正确 | `SELECT name FROM tags` | ✅ 数组 / 字符串 / 链表 / 栈/队列 / 树 / 图 / 动态规划 / 贪心 |
| AC-3c | backend → mysql 网络通 | `mysqladmin ping -h mysql` | ✅ `mysqld is alive` |
| AC-4a | `/api/health` 返回信封格式（直连 8080） | `curl :8080/api/health` | ✅ `{code:0, message:"ok", data:{status,version,uptime_ms,now_unix}}` |
| AC-4b | `/api/health` 经 nginx 反代（80） | `curl :80/api/health` | ✅ 同上 envelope |
| AC-4c | 后端进程可执行 + logs 正常 | `docker compose logs backend` | ✅ `oj_backend 1.0.0 listening on 0.0.0.0:8080 (threads=8)` |

**修复记录**（验证过程中发现并就地修复）：

1. **`libmysqlclient21` 在 Debian bookworm 不存在** → 替换为 `libmariadb3`
   （bookworm 的 mysqlclient 是 MariaDB 兼容实现）。
   影响文件：`backend/Dockerfile` runtime 阶段。
2. **CMakeLists 缺 install 规则**，`cmake --install build` 不会拷贝任何产物到 `/install/`。
   补充 `install(TARGETS oj_backend RUNTIME)` 与 `install(FILES default.json)`。
   影响文件：`backend/CMakeLists.txt`。
3. **Dockerfile 未拷贝 `backend/.local-deps/`，离线环境 FetchContent 拉 GitHub 必败**。
   显式 `COPY .local-deps/ ./.local-deps/` 并在 `cmake` 调用处追加
   `-DOJ_LOCAL_DEPS_DIR=/src/.local-deps`，让构建走本地路径（与 README 的"离线构建"指引一致）。
   影响文件：`backend/Dockerfile`。
4. **backend 镜像未装 `curl` CLI**，容器内 healthcheck 失败 → 服务 unhealthy。
   runtime 阶段追加 `curl` 包。
   影响文件：`backend/Dockerfile`。

**结论**：Phase 1 基础骨架通过验收，进入 Phase 2（账户系统）开发。

---

### 9.6 Phase 2 — users 表 / Argon2 密码哈希（已通过）
> 触发条件：SPEC §8 TODO「Phases 2 - 账户系统」第 1 项已交付。
> 本节为该项的**端到端验收报告**，详细命令、原始输出与修复记录见
> [`docs/phase2-verification.md`](docs/phase2-verification.md)。

**验证时间**：2026-06-16
**验证环境**：Linux x86_64 / GCC 13.3 / Debian 12 (bookworm) / libargon2-dev 0~20190702

**交付物**：

| # | 验收点 | 证据 | 结果 |
|---|---|---|---|
| 2-1a | `users` 表 schema 与 §4.2 完全一致 | `001_init.sql:16-27` 字段 / 类型 / 约束 / 字符集 / 引擎 / 幂等全部对齐 | ✅ |
| 2-1b | `PasswordHasher` API（hash/verify/is_encoded_hash）可用 | `infra/password_hasher.{hpp,cpp}` + 25 项单元测试 | ✅ |
| 2-1c | 输出 PHC 编码 `$argon2id$v=19$m=...,t=...,p=...$salt$hash` | `HashEmitsPhcEncodedArgon2id` 测试 + smoke 输出 | ✅ |
| 2-1d | 同密码两次 hash 输出不同（盐随机性） | `SamePasswordProducesDifferentHashes` 跑 8 次两两不等 | ✅ |
| 2-1e | encoded 长度 ≤ 255（兼容 VARCHAR(255)） | 实测 97 B，`EncodedLengthFitsVarchar255` 5 轮验证 | ✅ |
| 2-1f | verify 篡改 / 异常 / 空 / 过大输入均安全返 false（noexcept） | 6 项 `VerifyRejects*` 测试覆盖 | ✅ |
| 2-1g | 构造期参数 fail-fast | 3 项 `ConstructorRejects*` 测试 | ✅ |
| 2-1h | 60 项单元测试全过 | `./build/oj_unit_tests` PASSED 60/60 | ✅ |
| 2-1i | SPEC §9.3 S-2 密码 Argon2id 存储 / 不可逆 | 全部子项满足（见 phase2-verification §6） | ✅ |

**实现文件清单**：

| 文件 | 类型 | 说明 |
|---|---|---|
| `backend/include/infra/password_hasher.hpp` | 新增 | `PasswordHasher` 类 + `HashError` + `Params` |
| `backend/src/infra/password_hasher.cpp` | 新增 | libargon2 + OpenSSL `RAND_bytes` 实现 |
| `backend/tests/test_auth.cpp` | 新增 | 25 项 GoogleTest |
| `docs/phase2-verification.md` | 新增 | 本次验收报告 |

**结论**：Phase 2 第 1 项交付完成，进入第 2 项 `/api/auth/register` 实现首注册为 admin 逻辑。

---

### 9.7 Phase 2 — /api/auth/register 实现首注册为 admin 逻辑（已通过）
> 触发条件：SPEC §8 TODO「Phases 2 - 账户系统」第 2 项已交付。
> 本节为该项的**端到端验收报告**，详细命令、原始输出与单元测试矩阵见
> [`docs/phase3-verification.md`](docs/phase3-verification.md)。

**验证时间**：2026-06-16
**验证环境**：Linux x86_64 / GCC 13.3 / Debian 12 (bookworm) / libmysqlclient 21.2.46 / libargon2 / jwt-cpp 0.7
**交付物**：

| # | 验收点 | 证据 | 结果 |
|---|---|---|---|
| 2-2a | `POST /api/auth/register` 接口契约正确 | 响应 data = `{user_id, access_token, is_admin}` + `Set-Cookie: refresh_token=...; HttpOnly; SameSite=Lax; Path=/api/auth` | ✅ |
| 2-2b | 字段校验（username 3-20 / `[A-Za-z0-9_]`；email 含 @+.；password ≥ 8） | AuthServiceTest 10 项 + AuthHandlerTest 3 项 + E2E TEST E/F | ✅ |
| 2-2c | 重复 username/email → 1005 Conflict | AuthHandlerTest 2 项 + E2E TEST D | ✅ |
| 2-2d | **首注册用户 is_admin=true；后续 is_admin=false** | E2E TEST A/B + AuthServiceTest.FirstUserAcrossManyRegistrations（10 连跑）+ MysqlRepoTest 2 项 | ✅ |
| 2-2e | **并发首注册：恰 1 个 admin** | MysqlRepoTest.ConcurrentFirstRegistrationHasExactlyOneAdmin（8 并发） | ✅ |
| 2-2f | **InnoDB deadlock 自动重试** | MysqlRepoTest.DeadlockIsRetriedTransparently（pool_size=2 + 8 线程） | ✅ |
| 2-2g | MySQL 不可用 → 1008 SystemError | AuthHandlerTest.DbDownReturnsEnvelope | ✅ |
| 2-2h | JWT HS256 签发 + 验证（uid/adm/typ/iss/exp 完整） | JwtServiceTest 26 项（含篡改 / 过期 / 类型错） | ✅ |
| 2-2i | 密码以 Argon2id 存储（复用 Phase 2） | AuthServiceTest.PasswordIsHashedNotStoredAsPlaintext | ✅ |
| 2-2j | SQL 注入防御（escape / 参数化） | MysqlClientTest 10 项 escape + MysqlRepoTest.EscapeHandlesQuotesAndBackslashes | ✅ |
| 2-2k | 175 / 175 单元测试全过 | `./build/oj_unit_tests` 稳定通过 3 次 | ✅ |
| 2-2l | SPEC §9.4 M-1 分层（Http→Domain←Infra） | IUserRepository 接口在 domain、实现在 infra；handler 不直接依赖 mysql_client | ✅ |

**实现文件清单**：

| 文件 | 类型 | 说明 |
|---|---|---|
| `backend/include/common/config.hpp` | 修改 | 新增 MysqlConfig / JwtConfig |
| `backend/src/common/config.cpp` | 修改 | 解析 mysql / jwt 段 |
| `backend/include/infra/mysql_client.{hpp,cpp}` | 新增 | libmysqlclient 连接池 + RAII Lease |
| `backend/include/infra/jwt_service.{hpp,cpp}` | 新增 | HS256 颁发/验证 |
| `backend/include/infra/user_repo.{hpp,cpp}` | 新增 | MysqlUserRepo + deadlock retry |
| `backend/include/domain/user_repository.hpp` | 新增 | IUserRepository 接口（依赖倒置） |
| `backend/include/domain/auth_service.{hpp,cpp}` | 新增 | AuthService::register_user |
| `backend/include/http/handlers/auth_handler.{hpp,cpp}` | 新增 | POST /api/auth/register 路由 |
| `backend/src/main.cpp` | 修改 | 装配链路 + 注入路由 |
| `backend/tests/test_jwt_service.cpp` | 新增 | 26 项 JwtService 单测 |
| `backend/tests/test_mysql_client.cpp` | 新增 | 24 项 MysqlClient 单测 |
| `backend/tests/test_auth_service.cpp` | 新增 | 20 项 AuthService 单测 |
| `backend/tests/test_auth_handler.cpp` | 新增 | 11 项 AuthHandler E2E |
| `backend/tests/test_user_repo_mysql.cpp` | 新增 | 9 项 MysqlUserRepo（含 3 项并发） |
| `docs/phase3-verification.md` | 新增 | 本次验收报告 |

**结论**：Phase 2 第 2 项 `/api/auth/register 实现首注册为 admin 逻辑` 通过验收，
进入下一项 `/api/auth/login + JWT 颁发`。

---

## 附录 A：术语表

| 术语 | 含义 |
|---|---|
| OJ | Online Judge，在线评测系统 |
| 提交 (Submission) | 用户对一道题的一次代码提交 |
| 测试点 (Testcase) | 评测单个输入/预期输出对 |
| 样例点 (Sample) | 题面展示的测试点，提交结果会显示详情 |
| 隐藏点 (Hidden) | 不在题面展示的测试点，结果仅显示状态 |
| AC / WA / TLE / MLE / OLE / RE / CE / SE | 8 种判题终态（见 2.3.2） |
| SPJ | Special Judge，自定义判题程序（v1 不实现） |
| 沙箱 (Sandbox) | 隔离运行环境，本项目用 Docker |

