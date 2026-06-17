# Phase 5 验收报告 — 测试点动态增删 + 校验总分 = 100

> 对应 SPEC §8「Phases 5 - 后台管理」第 3 项：
> `[ ] 测试点动态增删 + 校验总分 = 100`
>
> 本阶段交付物：
> 1. **测试点动态增删行**（SPEC §2.5 / §3.3.5 M）
>    - 表格行：输入 / 预期输出 / 样例 / 分值 / 删除
>    - `+ 添加测试点` 按钮位于末尾
>    - 至少保留 1 个；删除时确认模态
> 2. **总分实时校验**（SPEC §2.5 / §3.3.5 M / AC-6）
>    - 底部 summary：`总分: X / 100` 实时计算
>    - 颜色态：ok→绿 / err→红
>    - **总分 ≠ 100 时 [保存草稿] / [发布] 按钮 disabled**（AC-6）
> 3. **"校验总分" 按钮**（SPEC §2.5）
>    - 点击 → toast 提示 ok / err
>    - err 时若有 firstBad 行 → 滚动 + 1.5s 高亮闪烁
> 4. **27 项 `validateTotal` 单元测试全过**（pure function，可独立 import）
>
> 验收时间：2026-06-17
> 环境：Node 22 / Debian 12 (bookworm)
> 结果：**通过** ✅

---

## 1. 验收范围

按 SPEC §2.5 / §3.3.5 M / §9.1.2 验证以下事实：

- [x] **AC-5**：admin 在后台创建一道题，题面 Markdown 实时预览正确（Phase 5 前置已实现）
- [x] **AC-6**：测试点 score 之和不等于 100 时提交按钮禁用（**本阶段新增**）
- [x] **SPEC §2.5**：测试点管理支持动态增删行
- [x] **SPEC §2.5**：「校验总分」按钮可用，前端校验所有 score 之和 = 100
- [x] **SPEC §3.3.5 M**：总分实时计算；≠ 100 时 [保存]/[发布] 按钮 disabled + 红色提示
- [x] **SPEC §3.3.5 M**：测试点 score 必填且 ≥ 0；"添加"按钮置于末尾
- [x] **后端兜底**：`ProblemService::validate_cases` 仍以 `sum=100` 二次校验（已有）

---

## 2. 实现细节

### 2.1 关键文件

| 文件 | 类型 | 说明 |
|---|---|---|
| `frontend/js/utils/problem-cases.js` | 新增 | **纯函数** `validateTotal(cases)` / 常量 / `normalizeScores`；无 DOM 依赖，可单测 |
| `frontend/js/views/admin-problem-edit.js` | 修改 | 集成 `validateTotal` + 实时 `updateTotalDisplay` + 校验按钮 + summary UI |
| `frontend/css/admin-page.css` | 修改 | `.ape-cases-summary*` / `.ape-cases-validate` / `.ape-case-row--bad` 样式 |
| `frontend/tests/problem-cases.test.mjs` | 新增 | 27 项 validateTotal / normalizeScores 单元测试 |

### 2.2 `validateTotal` 设计

```js
// utils/problem-cases.js
export const kCaseMin = 1;          // SPEC §2.2.1: 至少 1 个
export const kCaseMax = 100;        // SPEC §2.2.1: 最多 100 个
export const kCaseScoreSum = 100;   // SPEC §2.2.1: 总分 100
export const kCaseScoreMin = 0;
export const kCaseScoreMax = 100;

export function validateTotal(cases) {
    // → { ok, total, msg, firstBad }
}
```

**约束**（与后端 `ProblemService::validate_cases` 镜像一致）：
- 数量 ∈ [1, 100]
- 每个 score ∈ [0, 100] 且为整数
- `sum(score) === 100`

**返回值**（用于驱动 UI）：
- `ok` → 控制保存按钮 `disabled`
- `total` → 实时显示给用户看
- `msg` → 错误时给用户的中文提示（ok 时为 `null`）
- `firstBad` → 第一个出错行（per-row 错误时存在；sum 不等时为 `null`）

### 2.3 UI 行为（admin-problem-edit.js）

| 触发 | 行为 |
|---|---|
| 进入新建页 | 默认 1 个 case（score=0）→ 立即显示「⚠ 分数之和必须等于 100（当前 0）」+ 按钮 disabled |
| 修改某行 score | `c.score = v` → `markDirty()` → **`updateTotalDisplay()`**（仅刷新底部，不重渲染表格 → input 不会失焦） |
| 点击 `+ 添加测试点` | `state.cases.push({ score: 0 })` → `renderCasesSection()` 全量重建 → `updateTotalDisplay()` 触发 |
| 点击行末 ✕ | 至少保留 1 个（toast 提示）；确认后 `splice` + 重新编号 → `renderCasesSection()` → `updateTotalDisplay()` |
| 点击 `校验总分` | toast ok / err；err 且有 firstBad → 滚动 + 1.5s 红闪该行 |
| 总分 = 100 | summary 变绿 + 「总分已就绪，可以保存」+ 校验按钮置灰（no-op） |
| 总分 ≠ 100 | summary 变红 + 「⚠ 分数之和必须等于 100（当前 X）」+ 保存/发布按钮 disabled + 校验按钮可点 |

### 2.4 CSS 状态色

```css
.ape-cases-summary__total--ok   { /* green: rgba(74, 222, 128, .10) */ }
.ape-cases-summary__total--err  { /* red:   rgba(239, 68, 68, .10) */ }
.ape-cases-summary__msg--ok     { color: var(--ok);   }
.ape-cases-summary__msg--err    { color: var(--err); font-weight: 500; }
.ape-case-row--bad              { animation: ape-row-bad 1.5s; } /* 1.5s 红闪 */
```

### 2.5 后端兜底（已有，无需改动）

`backend/src/domain/problem_service.cpp:253-280` 已实现
`ProblemService::validate_cases`：
```cpp
if (score_sum != kProblemCaseScoreSum) {
    OJ_THROW_BAD("sum of all testcase scores must be 100, got " +
                 std::to_string(score_sum));
}
```
→ 即使前端有 bug 漏判，后端仍会以 1001 BadRequest 拒绝。

---

## 3. 单元测试矩阵（27 项）

`frontend/tests/problem-cases.test.mjs`（Node 22，无框架）：

| 类别 | 用例数 | 覆盖点 |
|---|---|---|
| 常量导出 | 1 | `kCaseMin/Max/ScoreSum/Min/Max` 值与 SPEC 一致 |
| 入参合法性 | 2 | `null` / `undefined` → err |
| 数量边界 | 5 | 0 / 1+100 / 1+99 / 1+101 / 100+1 / 101 |
| sum 不等 | 4 | 99 / 70 / 130 / 0（接近 100 也不模糊通过） |
| 单点非法 | 5 | -1 / NaN / 3.5 / "abc" / null |
| 累积 total | 1 | per-row 错误时 `total` 是已累计的（不重置为 0） |
| sum=100 典型 | 3 | 单点 100 / 双点 [50,50] / 5 点 [10,20,30,20,20] |
| 多点 sum=100 | 3 | 3 点 [30,30,40] / 10 点各 10 / 包含 0 分 |
| `normalizeScores` | 3 | 数字原样 / NaN/ undefined → 0 / 入参非数组 → `[]` |

```
# node tests/problem-cases.test.mjs
...
Passed: 27, Failed: 0
```

---

## 4. 现有测试回归

```
# for t in tests/*.test.mjs; do node "$t"; done
=== admin-problems.test.mjs ===   Passed: 19, Failed: 0
=== draft.test.mjs ===            Passed: 15, Failed: 0
=== poller.test.mjs ===           Passed: 22, Failed: 0
=== problem-cases.test.mjs ===    Passed: 27, Failed: 0   ← 新增
=== status-machine.test.mjs ===   Passed: 19, Failed: 0
                              ──────────────────────────
                                  Passed: 102, Failed: 0
```

JS 语法检查：
```
$ node --check frontend/js/views/admin-problem-edit.js
OK
$ node --check frontend/js/utils/problem-cases.js
OK
```

---

## 5. 端到端操作手册（手动验收步骤）

1. `docker compose up -d --build`，访问 `http://localhost/`
2. 注册 admin 账号（首注册自动为 admin）
3. 顶部导航 → 后台 → 新建题目
4. 填标题 `两数之和`、选难度 `易`、至少 1 个标签
5. 题面用默认模板
6. **测试点**区块：
   - 默认 1 行 score=0 → 底部「总分：0 / 100」红色 + 「⚠ 分数之和必须等于 100（当前 0）」
   - 右上「保存草稿」/「发布」按钮**置灰**，鼠标悬停提示「总分之和必须为 100 后才能保存」
7. 把第 1 行的分值改为 `100` → 立即变绿「总分已就绪，可以保存」+ 按钮可点
8. 点 `+ 添加测试点` → 多出 1 行 score=0，总分变 `100 / 100` 仍 ok
9. 把新行改为 `30` → 立即变红「⚠ 分数之和必须等于 100（当前 130）」+ 按钮置灰
10. 点「校验总分」→ toast「分数之和必须等于 100（当前 130）」+ 滚动到第 2 行 + 1.5s 红闪
11. 把新行改回 `0` → 总分恢复 100，按钮可点
12. 点「发布」→ 后端落库；新题进入题目列表

> 上述 1-12 步对应 SPEC §9.1.2 **AC-6** 的完整交互验证。

---

## 6. 结论

Phase 5「后台管理」三项全部完成：

- [x] admin API：CRUD + 上下架
- [x] 前端：管理后台题目列表 + Monaco Markdown 编辑器 + 客户端预览
- [x] **测试点动态增删 + 校验总分 = 100** ← 本次

**关键不变量**（SPEC §2.2.1）：
- 测试点数量 1–100 ✓
- 总分 = 100（前后端双校验）✓
- score ∈ [0, 100] 整数 ✓

进入 Phase 6（提交历史 + 详情）。
