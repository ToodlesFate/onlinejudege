// =============================================================================
//  utils/submission-detail-helpers.js — 提交详情页纯逻辑 (SPEC §3.3.5 K)
//
//  把 submission-detail.js 里**与 DOM 无关**的逻辑抽到本文件，便于单元测试
//  （submission-detail-helpers.test.mjs）。
//
//  覆盖：
//    1) 状态分类：EARLY_EXIT / FAIL_RESULTS
//    2) 文本 / 分数格式化
//    3) "测试点表格"空态文案选择
//    4) "meta 信息行"构造
//    5) 错点弹窗的"diff 列"构造 —— 样例点的 input/expected/user_output 三栏；
//       隐藏点：仅返回"为保护题目，不展示隐藏点详情"的占位
//    6) 行级 LCS diff —— 把 expected / user_output 按行比较，标 added/removed/same，
//       给 WA 的"你的输出"列做高亮渲染（前端调用方决定如何染色）
//    7) URL :id 解析
//
//  不依赖 DOM / window / document，可在 Node 单元测试里直接调用。
// =============================================================================

// ---------------------------------------------------------------------------
//  状态分类常量 (SPEC §2.3.2 / §3.3.5 K)
// ---------------------------------------------------------------------------

/** CE / SE 早退出 —— 没有测试点结果，SPEC §2.3.2 状态机 */
export const EARLY_EXIT_RESULTS = Object.freeze(['CE', 'SE']);

/** 错点 (WA / TLE / MLE / OLE / RE) —— SPEC §2.4 + §3.3.5 K 展示 diff */
export const FAIL_RESULTS = Object.freeze(['WA', 'TLE', 'MLE', 'OLE', 'RE']);

/** 8 态全部终态 */
export const ALL_TERMINAL_RESULTS = Object.freeze([
    'AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE',
]);

const EARLY_EXIT_SET = new Set(EARLY_EXIT_RESULTS);
const FAIL_SET       = new Set(FAIL_RESULTS);

/**
 * 结果是否属于"早退出"—— CE / SE
 * @param {string|null|undefined} result
 */
export function isEarlyExitResult(result) {
    return EARLY_EXIT_SET.has(result);
}

/**
 * 单点状态是否属于"错点"—— WA / TLE / MLE / OLE / RE
 * @param {string|null|undefined} status
 */
export function isFailStatus(status) {
    return FAIL_SET.has(status);
}

/**
 * 是否要展示"查看"按钮（错点 + 不是 CE/SE 早退出）。
 * SPEC §3.3.5 K: 错点 "查看" → 弹模态
 * @param {{ status?: string, is_sample?: boolean }} c
 */
export function shouldShowViewButton(c) {
    return !!(c && isFailStatus(c.status));
}

// ---------------------------------------------------------------------------
//  URL :id 解析
// ---------------------------------------------------------------------------

/**
 * 解析 path param :id → 正整数；非法返回 null。
 * @param {Record<string, string>|undefined|null} params
 */
export function parseSubmissionId(params) {
    const raw = params && params.id;
    if (raw == null) return null;
    const n = parseInt(String(raw), 10);
    if (!Number.isFinite(n) || n <= 0) return null;
    return n;
}

// ---------------------------------------------------------------------------
//  顶部条 / meta 信息
// ---------------------------------------------------------------------------

/**
 * 顶部右侧"总分 X / 100"
 * @param {number|null|undefined} total
 */
export function formatTotalScore(total) {
    const v = Number.isFinite(total) ? total : 0;
    return `总分 ${v} / 100`;
}

/**
 * 徽章显示哪个 code：未完成用 status（queued/compiling/running），已完成用 result。
 * @param {{ status?: string, result?: string|null }} detail
 */
export function pickStatusBadgeCode(detail) {
    if (!detail) return 'queued';
    if (detail.status === 'finished') return detail.result || 'SE';
    return detail.status || 'queued';
}

/**
 * 单点分数格式化（SPEC §3.3.5 K：表格中只展示数字本身，不带 "/100"）。
 * @param {number|null|undefined} score
 */
export function formatCaseScore(score) {
    const v = Number.isFinite(score) ? score : 0;
    return String(v);
}

/**
 * 测试点表格空态文案选择（SPEC §3.3.5 K + §2.4）。
 *   - CE  → "编译失败，无测试点运行"
 *   - SE  → "系统错误，无测试点结果"
 *   - 其他未完成 → "判题中…测试点结果即将到达"
 * @param {{ status?: string, result?: string|null, cases?: any[] }} detail
 */
export function emptyCasesMessage(detail) {
    const isEarly = isEarlyExitResult(detail && detail.result);
    if (isEarly) {
        return detail.result === 'CE'
            ? '编译失败，无测试点运行'
            : '系统错误，无测试点结果';
    }
    if (!detail || detail.status !== 'finished') {
        return '判题中…测试点结果即将到达';
    }
    return '暂无测试点';
}

/**
 * 把 detail 拍平成 [key, value][] —— 用于渲染 meta 信息行。
 * 用户 / 语言 / 耗时 / 内存 / 提交时间 / 判完时间
 *
 * @param {{
 *   username?: string|null, user_id?: number,
 *   language?: string,
 *   time_used_ms?: number|null, memory_used_kb?: number|null,
 *   created_at?: string|null, finished_at?: string|null,
 * }} detail
 * @param {{ label?: string }} [langInfo]
 * @returns {Array<[string, string]>}
 */
export function buildMetaItems(detail, langInfo) {
    const userDisp = (detail && detail.username)
        ? detail.username
        : (detail && detail.user_id ? `id:${detail.user_id}` : '—');
    const langDisp = (langInfo && langInfo.label)
        || (detail && detail.language)
        || '—';
    return [
        ['用户',     String(userDisp)],
        ['语言',     String(langDisp)],
        ['耗时',     String(detail && detail.time_used_ms != null ? detail.time_used_ms + ' ms' : '—')],
        ['内存',     String(detail && detail.memory_used_kb != null ? detail.memory_used_kb + ' KB' : '—')],
        ['提交时间', String(detail && detail.created_at || '—')],
        ['判完时间', String(detail && detail.finished_at || '—')],
    ];
}

// ---------------------------------------------------------------------------
//  错点弹窗 (SPEC §3.3.5 K)
// ---------------------------------------------------------------------------

/**
 * 构造错点弹窗的内容列。
 *   - 样例点 → 三列：输入 / 预期 / 你的输出
 *   - 隐藏点 → 单列：占位提示 "为保护题目，不展示隐藏点详情"
 *
 * @param {{
 *   is_sample?: boolean,
 *   input?: string|null, expected_output?: string|null, user_output?: string|null,
 *   status?: string,
 * }} c
 * @returns {{
 *   kind: 'sample' | 'hidden',
 *   columns: Array<{ title: string, text: string }>,
 *   hint?: string,
 * }}
 */
export function buildModalColumns(c) {
    const isSample = !!(c && c.is_sample);
    if (!isSample) {
        return {
            kind: 'hidden',
            columns: [],
            hint: '为保护题目，不展示隐藏点详情',
        };
    }
    return {
        kind: 'sample',
        columns: [
            { title: '输入 Input',  text: c.input           != null ? String(c.input)           : '' },
            { title: '预期输出',    text: c.expected_output != null ? String(c.expected_output) : '' },
            { title: '你的输出',    text: c.user_output     != null ? String(c.user_output)     : '' },
        ],
    };
}

// ---------------------------------------------------------------------------
//  行级 diff —— LCS 算法
//
//  输入：两段文本（多行）
//  输出：行数组 [{ kind: 'same'|'added'|'removed', text: string }]
//
//  用法：WA 错点的"你的输出"列可按 kind 高亮（绿 = 与预期一致，红 = 多出来的）
//        或"预期输出"列反向染色。
//
//  算法说明：
//    - 先按 \n split；保留尾部换行语义：连续两个空行视为同一空行
//    - LCS = Longest Common Subsequence；时间 O(m*n)，m/n 一般很小（< 200 行）
//    - 输出顺序：从前往后扫描，removed 在前，added 在后（diff 习惯）
// ---------------------------------------------------------------------------

/**
 * 把字符串切成行（保留尾空行）。
 * @param {string|null|undefined} s
 * @returns {string[]}
 */
export function splitLines(s) {
    if (s == null) return [];
    const str = String(s);
    if (str === '') return [];
    // splitlines-equivalent：保留空行；尾部换行算"末尾空行"，这里忽略它
    const lines = str.split(/\r?\n/);
    if (lines.length && lines[lines.length - 1] === '') lines.pop();
    return lines;
}

/**
 * 行级 LCS diff。
 * 输出顺序：从左到右扫描 expected；遇到不在 LCS 的行标 'removed'，
 *          遇到 actual 中不在 LCS 的行标 'added'。便于人眼对比。
 *
 * @param {string|null|undefined} expected
 * @param {string|null|undefined} actual
 * @returns {Array<{ kind: 'same'|'added'|'removed', text: string }>}
 */
export function computeLineDiff(expected, actual) {
    const a = splitLines(expected);
    const b = splitLines(actual);
    const m = a.length;
    const n = b.length;

    // 边界：任一为空
    if (m === 0 && n === 0) return [];
    if (m === 0) return b.map((t) => ({ kind: 'added', text: t }));
    if (n === 0) return a.map((t) => ({ kind: 'removed', text: t }));

    // dp[i][j] = LCS 长度
    const dp = new Array(m + 1);
    for (let i = 0; i <= m; ++i) dp[i] = new Array(n + 1).fill(0);
    for (let i = 1; i <= m; ++i) {
        for (let j = 1; j <= n; ++j) {
            if (a[i - 1] === b[j - 1]) dp[i][j] = dp[i - 1][j - 1] + 1;
            else dp[i][j] = Math.max(dp[i - 1][j], dp[i][j - 1]);
        }
    }

    // 回溯：从右下到左上
    /** @type {Array<{ kind: 'same'|'added'|'removed', text: string }>} */
    const out = [];
    let i = m, j = n;
    while (i > 0 && j > 0) {
        if (a[i - 1] === b[j - 1]) {
            out.push({ kind: 'same', text: a[i - 1] });
            i--; j--;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            out.push({ kind: 'removed', text: a[i - 1] });
            i--;
        } else {
            out.push({ kind: 'added', text: b[j - 1] });
            j--;
        }
    }
    while (i > 0) { out.push({ kind: 'removed', text: a[i - 1] }); i--; }
    while (j > 0) { out.push({ kind: 'added',   text: b[j - 1] }); j--; }
    out.reverse();
    return out;
}

/**
 * diff 摘要：去掉 same 行，只看差异。
 * @param {Array<{ kind: string }>} diffs
 * @returns {{ added: number, removed: number }}
 */
export function diffSummary(diffs) {
    let added = 0, removed = 0;
    for (const d of diffs) {
        if (d.kind === 'added')   added++;
        else if (d.kind === 'removed') removed++;
    }
    return { added, removed };
}