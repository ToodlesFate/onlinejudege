// =============================================================================
//  utils/problem-cases.js — 题目测试点的纯函数工具 (SPEC §2.2.1 / §2.5)
//
//  不依赖 DOM、不依赖 state，仅做"输入 → 输出"判断。
//  视图层（admin-problem-edit.js）复用同套逻辑来驱动
//    - 总分实时显示
//    - 保存/发布按钮 disabled
//    - 校验按钮提示文案
//
//  也可被单元测试直接 import（tests/problem-cases.test.mjs）。
// =============================================================================

/** 测试点最少 1 个、最多 100 个（SPEC §2.2.1） */
export const kCaseMin = 1;
export const kCaseMax = 100;

/** 所有测试点 score 之和必须等于 100（SPEC §2.2.1） */
export const kCaseScoreSum = 100;

/** 单个测试点 score 取值范围（SPEC §2.2.1） */
export const kCaseScoreMin = 0;
export const kCaseScoreMax = 100;

/**
 * @typedef {Object} TestCase
 * @property {number} case_index
 * @property {string} input
 * @property {string} expected_output
 * @property {boolean} is_sample
 * @property {number} score
 */

/**
 * @typedef {Object} ValidateTotalResult
 * @property {boolean} ok                  所有校验通过（可用于启用保存按钮）
 * @property {number}  total               当前 sum(score)（实时显示给用户看）
 * @property {?string} msg                 错误信息（中文）；ok=true 时为 null
 * @property {?{index:number,reason:string}} firstBad  第一个出错行的定位
 */

/**
 * 校验测试点数组：
 *   - 数量 ∈ [kCaseMin, kCaseMax]
 *   - 每个 score ∈ [kCaseScoreMin, kCaseScoreMax] 的整数
 *   - sum(score) === kCaseScoreSum
 *
 * 失败时返回 { ok:false, total, msg, firstBad }，firstBad 仅在 per-row
 * 错误（如某行 score 不是整数）时存在；sum 不等时 firstBad 为 null。
 *
 * @param {TestCase[]} cases
 * @returns {ValidateTotalResult}
 */
export function validateTotal(cases) {
    if (!Array.isArray(cases)) {
        return { ok: false, total: 0, msg: '测试点列表格式错误', firstBad: null };
    }
    const n = cases.length;
    if (n < kCaseMin) {
        return { ok: false, total: 0, msg: '请至少添加 ' + kCaseMin + ' 个测试点', firstBad: null };
    }
    if (n > kCaseMax) {
        return { ok: false, total: 0, msg: '测试点数量不可超过 ' + kCaseMax, firstBad: null };
    }

    let total = 0;
    for (let i = 0; i < n; ++i) {
        const c = cases[i] || {};
        const s = c.score;
        const v = Number(s);
        if (!Number.isFinite(v)
            || v < kCaseScoreMin
            || v > kCaseScoreMax
            || Math.floor(v) !== v) {
            return {
                ok: false,
                total: total,
                msg: '测试点 #' + (i + 1) + ' 的分值必须为 '
                    + kCaseScoreMin + '–' + kCaseScoreMax + ' 的整数',
                firstBad: { index: i, reason: 'score' },
            };
        }
        total += v;
    }
    if (total !== kCaseScoreSum) {
        return {
            ok: false,
            total: total,
            msg: '分数之和必须等于 ' + kCaseScoreSum + '（当前 ' + total + '）',
            firstBad: null,
        };
    }
    return { ok: true, total: total, msg: null, firstBad: null };
}

/**
 * 把 [{score}] 序列化为数字数组（默认空 -> 0），用于输入框 blur 时归一化。
 *
 * @param {TestCase[]} cases
 * @returns {number[]}
 */
export function normalizeScores(cases) {
    if (!Array.isArray(cases)) return [];
    return cases.map(c => {
        const v = Number(c && c.score);
        return Number.isFinite(v) ? v : 0;
    });
}
