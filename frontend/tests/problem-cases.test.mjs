// =============================================================================
//  tests/problem-cases.test.mjs — utils/problem-cases.js 单元测试
//  (Node, no framework)
//
//  覆盖 SPEC §2.2.1 / §2.5 / AC-6：
//    1) sum=100 → ok
//    2) sum<100 / sum>100 → err + 提示 "当前 X"
//    3) 单点 score 越界 (负数 / >100 / 非整数) → err + firstBad
//    4) 0 个 / 1 个 / 100 个 / 101 个 → 边界
//    5) 入参非数组 → err
//    6) normalizeScores 兜底
// =============================================================================

import assert from 'node:assert/strict';
import {
    validateTotal,
    normalizeScores,
    kCaseMin,
    kCaseMax,
    kCaseScoreSum,
    kCaseScoreMin,
    kCaseScoreMax,
} from '../js/utils/problem-cases.js';

// --- 极简 test runner ---
let pass = 0;
let fail = 0;
async function test(name, fn) {
    try {
        await fn();
        console.log('  OK  ', name);
        pass++;
    } catch (e) {
        console.error('  FAIL', name);
        console.error('       ', e && e.message);
        fail++;
    }
}
function mkCase(score, idx) {
    return { case_index: idx, input: '', expected_output: '', is_sample: false, score };
}

(async () => {
    console.log('# problem-cases: validateTotal / normalizeScores');

    // --- 导出常量 ---
    await test('常量：kCaseMin=1, kCaseMax=100, kCaseScoreSum=100, score∈[0,100]', () => {
        assert.equal(kCaseMin, 1);
        assert.equal(kCaseMax, 100);
        assert.equal(kCaseScoreSum, 100);
        assert.equal(kCaseScoreMin, 0);
        assert.equal(kCaseScoreMax, 100);
    });

    // --- 入参非法 ---
    await test('入参非数组 → err', () => {
        const r = validateTotal(null);
        assert.equal(r.ok, false);
        assert.equal(r.total, 0);
        assert.match(r.msg, /格式错误/);
    });
    await test('入参 undefined → err', () => {
        const r = validateTotal(undefined);
        assert.equal(r.ok, false);
    });

    // --- 数量边界 ---
    await test('0 个测试点 → err（提示至少 1）', () => {
        const r = validateTotal([]);
        assert.equal(r.ok, false);
        assert.match(r.msg, /至少/);
    });
    await test('1 个测试点 score=100 → ok', () => {
        const r = validateTotal([mkCase(100, 1)]);
        assert.equal(r.ok, true);
        assert.equal(r.total, 100);
    });
    await test('1 个测试点 score=99 → err', () => {
        const r = validateTotal([mkCase(99, 1)]);
        assert.equal(r.ok, false);
        assert.equal(r.total, 99);
        assert.match(r.msg, /当前 99/);
    });
    await test('1 个测试点 score=101 → err（单点越界）', () => {
        const r = validateTotal([mkCase(101, 1)]);
        assert.equal(r.ok, false);
        assert.equal(r.firstBad && r.firstBad.index, 0);
        assert.equal(r.firstBad && r.firstBad.reason, 'score');
    });

    // --- 100 个测试点边界 ---
    await test('100 个测试点 sum=100 → ok', () => {
        const cases = [];
        for (let i = 0; i < 100; ++i) cases.push(mkCase(1, i + 1));
        const r = validateTotal(cases);
        assert.equal(r.ok, true);
        assert.equal(r.total, 100);
    });
    await test('101 个测试点 → err（数量超限）', () => {
        const cases = [];
        for (let i = 0; i < 101; ++i) cases.push(mkCase(1, i + 1));
        const r = validateTotal(cases);
        assert.equal(r.ok, false);
        assert.match(r.msg, /不可超过/);
    });

    // --- sum 不等 ---
    await test('sum=130 → err + 提示 "当前 130"', () => {
        const r = validateTotal([mkCase(100, 1), mkCase(30, 2)]);
        assert.equal(r.ok, false);
        assert.equal(r.total, 130);
        assert.match(r.msg, /当前 130/);
    });
    await test('sum=70 → err + 提示 "当前 70"', () => {
        const r = validateTotal([mkCase(30, 1), mkCase(40, 2)]);
        assert.equal(r.ok, false);
        assert.equal(r.total, 70);
    });
    await test('sum=99 接近 100 → err（不模糊）', () => {
        const r = validateTotal([mkCase(99, 1), mkCase(0, 2)]);
        assert.equal(r.ok, false);
    });

    // --- 单点非法 ---
    await test('单点 score=-1 → err（firstBad.reason=score）', () => {
        const r = validateTotal([mkCase(-1, 1)]);
        assert.equal(r.ok, false);
        assert.equal(r.firstBad.reason, 'score');
    });
    await test('单点 score=NaN → err', () => {
        const r = validateTotal([{ case_index: 1, input: '', expected_output: '',
                                   is_sample: false, score: NaN }]);
        assert.equal(r.ok, false);
    });
    await test('单点 score=3.5 → err（非整数）', () => {
        const r = validateTotal([mkCase(3.5, 1)]);
        assert.equal(r.ok, false);
    });
    await test('单点 score="abc" → err', () => {
        const r = validateTotal([mkCase('abc', 1)]);
        assert.equal(r.ok, false);
    });
    await test('单点 score=null → err', () => {
        const r = validateTotal([mkCase(null, 1)]);
        assert.equal(r.ok, false);
    });

    // --- 累积 total 在 per-row 错误时返回已累计值 ---
    await test('第 2 行 score 越界 → total=第 1 行（=50）', () => {
        const r = validateTotal([mkCase(50, 1), mkCase(200, 2)]);
        assert.equal(r.ok, false);
        assert.equal(r.total, 50);
        assert.equal(r.firstBad.index, 1);
    });

    // --- 多行 sum=100（典型 case） ---
    await test('3 个 case [30,30,40] → ok', () => {
        const r = validateTotal([mkCase(30, 1), mkCase(30, 2), mkCase(40, 3)]);
        assert.equal(r.ok, true);
        assert.equal(r.total, 100);
    });
    await test('10 个 case 各 10 分 → ok', () => {
        const cases = [];
        for (let i = 0; i < 10; ++i) cases.push(mkCase(10, i + 1));
        const r = validateTotal(cases);
        assert.equal(r.ok, true);
        assert.equal(r.total, 100);
    });
    await test('多 case 中一个 0 分不影响 sum', () => {
        const r = validateTotal([mkCase(100, 1), mkCase(0, 2)]);
        assert.equal(r.ok, true);
    });

    // --- normalizeScores ---
    await test('normalizeScores 数字原样保留', () => {
        assert.deepEqual(normalizeScores([{ score: 30 }, { score: 70 }]), [30, 70]);
    });
    await test('normalizeScores NaN/undefined → 0', () => {
        assert.deepEqual(
            normalizeScores([{ score: 30 }, { score: NaN }, { score: undefined }, {}]),
            [30, 0, 0, 0]
        );
    });
    await test('normalizeScores 入参非数组 → []', () => {
        assert.deepEqual(normalizeScores(null), []);
        assert.deepEqual(normalizeScores(undefined), []);
    });

    // --- 模拟 SPEC §2.2.1 典型用例 ---
    await test('SPEC 典型 1：单点 100', () => {
        const r = validateTotal([mkCase(100, 1)]);
        assert.equal(r.ok, true);
    });
    await test('SPEC 典型 2：双点 [50,50]', () => {
        const r = validateTotal([mkCase(50, 1), mkCase(50, 2)]);
        assert.equal(r.ok, true);
    });
    await test('SPEC 典型 3：5 个 case [10,20,30,20,20]', () => {
        const r = validateTotal([mkCase(10, 1), mkCase(20, 2), mkCase(30, 3), mkCase(20, 4), mkCase(20, 5)]);
        assert.equal(r.ok, true);
        assert.equal(r.total, 100);
    });

    console.log('---');
    console.log(`Passed: ${pass}, Failed: ${fail}`);
    process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
