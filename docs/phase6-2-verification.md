# Phase 6-2 验收报告 — 提交详情：Monaco 只读 + 逐点状态表格 + 错点 diff

> 对应 SPEC §8「Phases 6 - 提交历史 + 详情」第 2 项：
> `[ ] 提交详情：Monaco 只读 + 逐点状态表格 + 错点 diff`

## 1. 验收范围

按 SPEC §2.4 / §3.3.5 K / §9.1.5（AC-19）验证以下事实：

- [x] 页面 `/submissions/:id` 渲染：顶部条 / 元信息 / 状态机 / Tab 切换（源代码 / 测试点）/ 测试点表 / 错点弹窗
- [x] **Monaco 只读模式**（失败降级 textarea）：代码区使用 `createEditorOrFallback({ readOnly: true })`
- [x] **逐点状态表格**：# / 状态 / 耗时 / 内存 / 分数 / 类型（样例/隐藏）/ 详情
- [x] **错点"查看" → diff 弹窗**
  - 样例点：三列（input / expected / user_output），WA 时 user_output 列内嵌 **LCS 行级 diff 高亮**
  - 隐藏点：仅显示 "为保护题目，不展示隐藏点详情"
- [x] **AC-19**：错点详情展示 `user_output`（仅 is_sample=1）/ `expected` / diff
- [x] **CE 终态**：源代码区下方展示 `compile_output`（保留换行）
- [x] **SE 终态**：展示 `judge_message`
- [x] **公开访问**：仅 AC 对匿名 / 其他用户可见；非 AC 仅本人 / admin 可见
- [x] **403 / 404 兜底**：toast + 回上一页 / 返回提交列表
- [x] **2s 轮询**：未完成时每 2s 拉一次，30min 超时停，finished 立即停
- [x] 切 tab 至后台不暂停（SPEC §2.3.4 简化实现）

## 2. 交付物

### 2.1 新增 / 修改文件

| 文件 | 类型 | 说明 |
|---|---|---|
| `frontend/js/utils/submission-detail-helpers.js` | 新增 | 提交详情页纯逻辑：状态分类 / URL 解析 / 文本格式化 / 弹窗列构造 / **LCS 行级 diff** |
| `frontend/js/views/submission-detail.js` | 重构 | 接入 helpers；增加 WA 时 user_output 列的 LCS 行级 diff 高亮；移除冗余的 EARLY_EXIT / FAIL 常量 |
| `frontend/css/submission-page.css` | 修改 | 新增 `.sd-diff-line--same/added/removed` / `.sd-diff-col__pre--user` / `.sd-diff-summary` |
| `frontend/tests/submission-detail-helpers.test.mjs` | 新增 | **50 项** 纯逻辑测试 |

后端 `GET /api/submissions/:id` 已具备完整数据形状（SPEC §5.3），无需修改：

```json
{
  "code": 0,
  "data": {
    "id": 5, "problem_id": 1000, "user_id": 2, "username": "alice",
    "language": "cpp", "code": "#include<...>",
    "status": "finished", "result": "WA",
    "total_score": 30, "time_used_ms": 15, "memory_used_kb": 1024,
    "compile_output": "", "judge_message": "",
    "created_at": "...", "finished_at": "...",
    "cases": [
      { "case_index": 1, "status": "WA", "is_sample": true,  "score": 0,  "input": "1 2\n",   "expected_output": "3\n",   "user_output": "2\n" },
      { "case_index": 2, "status": "AC", "is_sample": true,  "score": 30, "input": "10 20\n", "expected_output": "30\n",  "user_output": "30\n" },
      { "case_index": 3, "status": "WA", "is_sample": false, "score": 0,  "input": null,     "expected_output": null,   "user_output": null }
    ]
  }
}
```

## 3. 关键实现要点

### 3.1 纯逻辑拆分 —— `submission-detail-helpers.js`

为了让 view 的 DOM 操作便于测试，把与 DOM 无关的逻辑抽到独立 ESM：

| 函数 | 用途 |
|---|---|
| `parseSubmissionId(params)` | 把 path `:id` 解析成正整数；非法返回 null |
| `pickStatusBadgeCode(detail)` | 选徽章 code：`finished` 用 `result`，其他用 `status` |
| `formatTotalScore(score)` | "总分 X / 100" |
| `formatCaseScore(score)` | **只返数字本身**（如 `"30"`，不带 "/100"；修复原表格 bug） |
| `emptyCasesMessage(detail)` | CE → "编译失败，无测试点运行" / SE → "系统错误" / 其他未完成 → "判题中…" |
| `buildMetaItems(detail, langInfo)` | 返回 6 行 meta：`[['用户', 'alice'], ['语言', 'C++'], ...]` |
| `buildModalColumns(c)` | 弹窗列构造：样例 → 三列；隐藏 → 占位 hint |
| `isFailStatus(s)` / `isEarlyExitResult(r)` / `shouldShowViewButton(c)` | 状态分类 |
| `splitLines(s)` / `computeLineDiff(exp, act)` / `diffSummary(diffs)` | LCS 行级 diff |

### 3.2 LCS 行级 diff —— `computeLineDiff`

经典 LCS（Longest Common Subsequence）算法：

```
expected: "1\n2\n3\n"
actual:   "1\n2\n5\n6\n"
                       ↓ LCS
output:   [ {kind:'same', text:'1'},
            {kind:'same', text:'2'},
            {kind:'removed', text:'3'},
            {kind:'added', text:'5'},
            {kind:'added', text:'6'} ]
```

- 时间复杂度 O(m·n)，m/n 通常 < 200 行（文本测试点），足够快
- 边界：两侧为空 → `[]`；一侧为空 → 全 `added` / `removed`；完全相同 → 全 `same`
- 唯一化展示：`splitLines` 去掉尾换行产生的空行，避免 "expected=3\n, actual=3" 被误判为多 1 行

### 3.3 WA 错点的 diff 高亮渲染

```js
if (isWa) {
    grid.appendChild(makeDiffColFromDiff(
        '你的输出',
        modal.columns[1].text,   // expected
        modal.columns[2].text,   // actual
    ));
}
```

`makeDiffColFromDiff` 把每行包成 `<span class="sd-diff-line sd-diff-line--same|--added|--removed">`，CSS 染色：

```css
.sd-diff-line--same    { color: var(--fg-3); background: transparent; }
.sd-diff-line--added   { color: var(--fg-0); background: rgba(248,113,113,.18);
                         border-left: 2px solid var(--st-wa); padding-left: 4px; }
```

并在弹窗底部追加一行 diff 摘要："行级 diff：共 N 行，匹配 M 行"。

> **设计取舍**：TLE / MLE / OLE / RE 没有 user_output 与 expected 的"内容差异"，但仍展示三列方便用户排查；WA 是唯一触发 LCS 高亮的状态。

### 3.4 表格 score bug 修复

| 旧实现 | 新实现 |
|---|---|
| `\`${c.score \|\| 0} / 100\`` → "30 / 100" | `formatCaseScore(c.score)` → "30" |

SPEC §3.3.5 K 表格示例：
```
│  1 │ [AC] │ 10ms │ 4MB  │ 30   │ [查看] (样例)  │
│  2 │ [AC] │ 12ms │ 4MB  │ 30   │ (隐藏)         │
│  3 │ [WA] │ 50ms │ 8MB  │ 0    │ [查看] (隐藏)  │
```
分数列只展示数字本身（30 / 30 / 0），不带 "/100"。新实现符合 SPEC。

### 3.5 轮询行为（沿用 SPEC §2.3.4）

- 进入页面 → 立即拉一次 `GET /api/submissions/:id`
- `status === 'finished'` → 不轮询
- 否则 → `createPoller({ intervalMs: 2000, maxDurationMs: 30min, shouldStop: data => data.status === 'finished' })`
- 终态 → toast "判题完成"
- 超时 → toast "判题超时（> 30 分钟）"
- 离开页面 → 调 `root._cleanup()`：停 poller + dispose Monaco

## 4. 测试覆盖

### 4.1 前端 —— `submission-detail-helpers.test.mjs`（50 项全过）

```
✓ EARLY_EXIT_RESULTS / FAIL_RESULTS / ALL_TERMINAL_RESULTS 三常量内容
✓ isEarlyExitResult / isFailStatus / shouldShowViewButton 状态分类
✓ parseSubmissionId 边界（正整数 / 0 / 负数 / NaN / null）
✓ pickStatusBadgeCode（finished→result / 未完成→status / null→queued）
✓ formatTotalScore + formatCaseScore + 空态文案 + meta items 顺序与 fallback
✓ buildModalColumns（样例三列 / 隐藏点占位 / null 字段→空文本）
✓ splitLines（普通 / 尾换行 / 空串 / \r\n）
✓ computeLineDiff 完全一致 / 多行 / 少行 / 完全不同 / 全空 / 嵌套差异 / 多行长文本
✓ diffSummary 统计
✓ AC-19 完整链路：样例 WA → 三列、样例 AC → 不显示查看、隐藏 WA → 隐藏占位
```

### 4.2 现有测试不回归

| 文件 | 通过 / 总数 |
|---|---|
| `submission-list.test.mjs` | 44 / 44 |
| `submission-detail-helpers.test.mjs` | **50 / 50**（新增） |
| `admin-problems.test.mjs` | 19 / 19 |
| `draft.test.mjs` | 15 / 15 |
| `draft.e2e.mjs` | E2E ALL PASSED |
| `poller.test.mjs` | 22 / 22 |
| `problem-cases.test.mjs` | 27 / 27 |
| `status-machine.test.mjs` | 19 / 19 |
| **小计** | **196 / 196** |

### 4.3 后端无回归

`./build/oj_unit_tests --gtest_brief=1`：**554 / 554 PASSED**（98 项 MySQL 集成测试需真实 DB，已 skip）。

## 5. 手动验证步骤

```bash
# 1) 启动服务
docker compose up -d --build

# 2) 注册 admin（首注册即 admin），登录拿到 token
ACCESS=$(curl -s -X POST http://localhost/api/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"password123"}' \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["data"]["access_token"])')

# 3) 创建带样例点的题
curl -s -X POST http://localhost/api/admin/problems \
  -H "Authorization: Bearer $ACCESS" -H 'Content-Type: application/json' \
  -d '{ "title":"两数之和", "content_md":"...", "difficulty":"easy",
        "tags":[1], "time_limit_ms":2000, "memory_limit_mb":256,
        "output_limit_mb":64, "is_published":true,
        "cases":[
          {"case_index":1,"input":"1 2\n","expected_output":"3\n","is_sample":true, "score":30},
          {"case_index":2,"input":"10 20\n","expected_output":"30\n","is_sample":true, "score":30},
          {"case_index":3,"input":"100 200\n","expected_output":"300\n","is_sample":false,"score":40}
        ] }'

# 4) 提交 WA 代码
curl -s -X POST http://localhost/api/submissions \
  -H "Authorization: Bearer $ACCESS" -H 'Content-Type: application/json' \
  -d '{"problem_id":1000,"language":"cpp","code":"...WA 实现..."}'

# 5) 查详情（含 cases + 三列样例输入）
curl -s "http://localhost/api/submissions/<id>" -H "Authorization: Bearer $ACCESS" | jq .

# 6) 浏览器访问
open http://localhost/submissions/<id>
#   - 顶部条：提交 #id + [WA] 徽章 + 总分 30/100
#   - 元信息：alice / C++ / 15ms / 1MB / 提交时间 / 判完时间
#   - 状态机：3 节点全 done
#   - Tab「测试点」：3 行
#     #1 [WA] 样例 [查看]   → 弹模态：input | expected | user_output (WA 时 user_output 列红/灰染色)
#     #2 [AC] 样例 —         → 不显示查看
#     #3 [WA] 隐藏 —         → 不显示查看（保护题目）
#   - Tab「源代码」：Monaco 只读（C++ 高亮）
```

## 6. 关键修复 / 增强

| # | 项目 | 描述 |
|---|---|---|
| 1 | **Bug 修复** | 表格 score 列从 "30 / 100" 改为 "30"（`formatCaseScore`） |
| 2 | **新功能** | WA 错点的 user_output 列 **LCS 行级 diff 高亮**（红 = 多出来 / 灰 = 一致） |
| 3 | **可测试性** | 抽出 9 个纯函数 + LCS diff 到 helpers.js；新增 50 项单测 |
| 4 | **CSS 增强** | 新增 4 个类（same / added / removed / summary），统一用 `--st-wa` 色板 |
| 5 | **可维护性** | view 文件不再持有 EARLY_EXIT / FAIL 常量，统一从 helpers 导入 |

## 7. 已知限制 / 后续工作

1. **不展示 removed 行**：当前 `makeDiffColFromDiff` 只用 `same` 和 `added` 染色（WA 视角：你的输出里多出来的行 = 错点）。`removed` 类已定义但暂未用到 —— 如果后续要支持双向对比（expected vs actual），可扩展。
2. **大文本性能**：LCS 是 O(m·n)，对超长输出（> 1000 行）会慢。判题用例通常 < 100 行，未优化；如出现性能问题可换 Myers diff。
3. **二进制 / 非 UTF-8 输出**：当前 LCS 按行 + 字符串完全相等比较，二进制输出（罕见）会被当成长度 1 的字符串；不影响正确性。
4. **服务端推送**：未来若引入 WebSocket / SSE，可去掉 2s 轮询（SPEC §2.3.4 已声明简化实现）。

## 8. 结论

Phase 6 第 2 项「提交详情：Monaco 只读 + 逐点状态表格 + 错点 diff」通过验收：

- ✅ Monaco 只读 + textarea 降级
- ✅ 逐点状态表格（# / 状态 / 耗时 / 内存 / 分数 / 类型 / 详情）
- ✅ 错点"查看" → 弹模态（样例三列 / 隐藏占位）
- ✅ **WA 时 LCS 行级 diff 高亮**（SPEC §2.4 "diff" 要求）
- ✅ CE / SE 终态专属区域（compile_output / judge_message）
- ✅ 公开访问规则：AC 公开、非 AC 仅本人 / admin
- ✅ 403 / 404 兜底
- ✅ 50 项前端纯逻辑测试全过
- ✅ 全部 196 项前端测试 + 554 项后端测试稳定通过
- ✅ JS 语法 / 静态资源加载 / Docker 服务全部正常

进入 Phase 6 第 3 项（如有）或 Phase 7「打磨与验收」。