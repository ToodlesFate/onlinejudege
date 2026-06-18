// =============================================================================
//  tests/submission-detail-helpers.test.mjs — 提交详情页纯逻辑测试 (无 DOM)
//
//  覆盖 SPEC §2.4 / §3.3.5 K 的关键纯逻辑：
//    1) 状态分类：EARLY_EXIT / FAIL_RESULTS / isFailStatus / isEarlyExitResult
//    2) URL :id 解析
//    3) 顶部条 / meta / 空态文案
//    4) 错点弹窗列构造（样例三列 vs 隐藏点占位）
//    5) LCS 行级 diff：完全一致 / 多出 / 少出 / 修改 / 空文本 / 多行
//    6) AC-19：错点详情展示 user_output（仅 sample）/ expected / diff
// =============================================================================

import assert from 'node:assert/strict';

import {
    EARLY_EXIT_RESULTS,
    FAIL_RESULTS,
    ALL_TERMINAL_RESULTS,
    isEarlyExitResult,
    isFailStatus,
    shouldShowViewButton,
    parseSubmissionId,
    pickStatusBadgeCode,
    formatTotalScore,
    formatCaseScore,
    emptyCasesMessage,
    buildMetaItems,
    buildModalColumns,
    splitLines,
    computeLineDiff,
    diffSummary,
} from '../js/utils/submission-detail-helpers.js';

// --- 极简 test runner ---
let pass = 0;
let fail = 0;
async function test(name, fn) {
    try {
        await fn();
        console.log(`  \u2713 ${name}`);
        pass++;
    } catch (e) {
        console.log(`  \u2717 ${name}`);
        console.log(`    ${e.stack || e.message}`);
        fail++;
    }
}

// =============================================================================
//  Tests
// =============================================================================
console.log('submission-detail-helpers tests:');

// ---- 状态分类 -------------------------------------------------------------
await test('EARLY_EXIT_RESULTS: \u5305\u542b CE / SE', () => {
    assert.ok(EARLY_EXIT_RESULTS.includes('CE'));
    assert.ok(EARLY_EXIT_RESULTS.includes('SE'));
    assert.equal(EARLY_EXIT_RESULTS.length, 2);
});

await test('FAIL_RESULTS: \u5305\u542b WA / TLE / MLE / OLE / RE', () => {
    for (const r of ['WA', 'TLE', 'MLE', 'OLE', 'RE']) {
        assert.ok(FAIL_RESULTS.includes(r));
    }
    assert.equal(FAIL_RESULTS.length, 5);
});

await test('ALL_TERMINAL_RESULTS: 8 \u6001\u5168\u90e8\u5305\u542b', () => {
    assert.equal(ALL_TERMINAL_RESULTS.length, 8);
    for (const r of ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE']) {
        assert.ok(ALL_TERMINAL_RESULTS.includes(r));
    }
});

await test('isEarlyExitResult: CE / SE \u2192 true', () => {
    assert.equal(isEarlyExitResult('CE'), true);
    assert.equal(isEarlyExitResult('SE'), true);
});

await test('isEarlyExitResult: WA / AC / null / undefined \u2192 false', () => {
    assert.equal(isEarlyExitResult('WA'),  false);
    assert.equal(isEarlyExitResult('AC'),  false);
    assert.equal(isEarlyExitResult(null), false);
    assert.equal(isEarlyExitResult(undefined), false);
    assert.equal(isEarlyExitResult(''), false);
});

await test('isFailStatus: WA / TLE / MLE / OLE / RE \u2192 true', () => {
    assert.equal(isFailStatus('WA'),  true);
    assert.equal(isFailStatus('TLE'), true);
    assert.equal(isFailStatus('MLE'), true);
    assert.equal(isFailStatus('OLE'), true);
    assert.equal(isFailStatus('RE'),  true);
});

await test('isFailStatus: AC / CE / SE / queued \u2192 false', () => {
    assert.equal(isFailStatus('AC'), false);
    assert.equal(isFailStatus('CE'), false);
    assert.equal(isFailStatus('SE'), false);
    assert.equal(isFailStatus('queued'), false);
});

await test('shouldShowViewButton: \u9519\u70b9 \u2192 true', () => {
    assert.equal(shouldShowViewButton({ status: 'WA',  is_sample: true }),  true);
    assert.equal(shouldShowViewButton({ status: 'WA',  is_sample: false }), true);
    assert.equal(shouldShowViewButton({ status: 'TLE', is_sample: false }), true);
    assert.equal(shouldShowViewButton({ status: 'RE',  is_sample: true }),  true);
});

await test('shouldShowViewButton: AC / CE / SE \u2192 false', () => {
    assert.equal(shouldShowViewButton({ status: 'AC', is_sample: true }), false);
    assert.equal(shouldShowViewButton({ status: 'CE', is_sample: true }), false);
    assert.equal(shouldShowViewButton({ status: 'SE', is_sample: true }), false);
});

await test('shouldShowViewButton: null / undefined \u2192 false', () => {
    assert.equal(shouldShowViewButton(null), false);
    assert.equal(shouldShowViewButton(undefined), false);
});

// ---- URL :id 解析 ----------------------------------------------------------
await test('parseSubmissionId: \u6b63\u6574\u6570 \u2192 \u6570\u5b57', () => {
    assert.equal(parseSubmissionId({ id: '123' }), 123);
    assert.equal(parseSubmissionId({ id: '1' }),   1);
});

await test('parseSubmissionId: 0 / \u8d1f\u6570 / NaN / null \u2192 null', () => {
    assert.equal(parseSubmissionId({ id: '0' }),    null);
    assert.equal(parseSubmissionId({ id: '-1' }),   null);
    assert.equal(parseSubmissionId({ id: 'abc' }),  null);
    assert.equal(parseSubmissionId({ id: '' }),     null);
    assert.equal(parseSubmissionId({}),             null);
    assert.equal(parseSubmissionId(null),           null);
    assert.equal(parseSubmissionId(undefined),      null);
});

// ---- 顶部 / 状态徽章 -------------------------------------------------------
await test('pickStatusBadgeCode: finished+AC \u2192 AC', () => {
    assert.equal(pickStatusBadgeCode({ status: 'finished', result: 'AC' }), 'AC');
});

await test('pickStatusBadgeCode: finished+WA \u2192 WA', () => {
    assert.equal(pickStatusBadgeCode({ status: 'finished', result: 'WA' }), 'WA');
});

await test('pickStatusBadgeCode: finished+\u65e0 result \u2192 SE \u8d8a\u91cf', () => {
    assert.equal(pickStatusBadgeCode({ status: 'finished' }), 'SE');
});

await test('pickStatusBadgeCode: running / compiling / queued \u2192 \u672c\u8eab', () => {
    assert.equal(pickStatusBadgeCode({ status: 'queued' }),    'queued');
    assert.equal(pickStatusBadgeCode({ status: 'compiling' }), 'compiling');
    assert.equal(pickStatusBadgeCode({ status: 'running' }),   'running');
});

await test('pickStatusBadgeCode: null \u2192 queued \u5b89\u5168\u964d\u7ea7', () => {
    assert.equal(pickStatusBadgeCode(null), 'queued');
});

await test('formatTotalScore: \u6570\u5b57 + "/ 100"', () => {
    assert.equal(formatTotalScore(100), '\u603b\u5206 100 / 100');
    assert.equal(formatTotalScore(0),   '\u603b\u5206 0 / 100');
    assert.equal(formatTotalScore(60),  '\u603b\u5206 60 / 100');
});

await test('formatTotalScore: null / undefined \u2192 0', () => {
    assert.equal(formatTotalScore(null),      '\u603b\u5206 0 / 100');
    assert.equal(formatTotalScore(undefined), '\u603b\u5206 0 / 100');
});

await test('formatCaseScore: \u53ea\u8fd4\u6570\u5b57\u672c\u8eab\uff0c\u4e0d\u5e26 "/100" (\u539f\u8868\u683c bugfix)', () => {
    assert.equal(formatCaseScore(30),  '30');
    assert.equal(formatCaseScore(0),   '0');
    assert.equal(formatCaseScore(100), '100');
});

await test('formatCaseScore: null / undefined / NaN \u2192 "0"', () => {
    assert.equal(formatCaseScore(null),      '0');
    assert.equal(formatCaseScore(undefined), '0');
});

// ---- \u7a7a\u6001\u6587\u6848 ----------------------------------------------------------
await test('emptyCasesMessage: CE \u2192 "\u7f16\u8bd1\u5931\u8d25"', () => {
    assert.match(emptyCasesMessage({ result: 'CE' }), /\u7f16\u8bd1\u5931\u8d25/);
});

await test('emptyCasesMessage: SE \u2192 "\u7cfb\u7edf\u9519\u8bef"', () => {
    assert.match(emptyCasesMessage({ result: 'SE' }), /\u7cfb\u7edf\u9519\u8bef/);
});

await test('emptyCasesMessage: \u672a\u5b8c\u6210 \u2192 "\u5224\u9898\u4e2d"', () => {
    assert.match(emptyCasesMessage({ status: 'queued' }),   /\u5224\u9898\u4e2d/);
    assert.match(emptyCasesMessage({ status: 'compiling' }),/\u5224\u9898\u4e2d/);
    assert.match(emptyCasesMessage({ status: 'running' }),  /\u5224\u9898\u4e2d/);
});

await test('emptyCasesMessage: finished + \u6709 cases \u4e0d\u4f1a\u88ab\u8c03\u7528', () => {
    // \u5b9e\u9645\u4e0a\u8c03\u7528\u65f6\u4e0d\u4f1a\u4f20\u8fdb\u6765\uff08\u8868\u683c\u6709\u6570\u636e\uff09\uff0c
    // \u4f46\u51fd\u6570\u672c\u8eab\u5e94\u8be5\u8fd4\u4e00\u4e2a\u9ed8\u8ba4\u6587\u6848\u4ee5\u9632\u4f7f\u7528\u8005\u8c03\u9519
    const m = emptyCasesMessage({ status: 'finished', result: 'AC' });
    assert.ok(typeof m === 'string' && m.length > 0);
});

// ---- meta items ------------------------------------------------------------
await test('buildMetaItems: 6 \u884c\uff0c\u987a\u5e8f \u7528\u6237/\u8bed\u8a00/\u8017\u65f6/\u5185\u5b58/\u63d0\u4ea4/\u5224\u5b8c', () => {
    const items = buildMetaItems({
        username: 'alice', user_id: 1, language: 'cpp',
        time_used_ms: 15, memory_used_kb: 4096,
        created_at: '2026-04-23T10:00:00Z', finished_at: '2026-04-23T10:00:15Z',
    }, { label: 'C++' });
    assert.equal(items.length, 6);
    assert.deepEqual(items.map(x => x[0]),
        ['\u7528\u6237', '\u8bed\u8a00', '\u8017\u65f6', '\u5185\u5b58', '\u63d0\u4ea4\u65f6\u95f4', '\u5224\u5b8c\u65f6\u95f4']);
});

await test('buildMetaItems: username \u4f18\u5148\uff1b\u7f3a\u5931\u65f6 id:N', () => {
    const items = buildMetaItems({ username: 'alice', user_id: 1, language: 'cpp',
        time_used_ms: 0, memory_used_kb: 0, created_at: 'x', finished_at: 'y' }, {});
    assert.equal(items[0][1], 'alice');

    const items2 = buildMetaItems({ username: '', user_id: 7, language: 'cpp',
        time_used_ms: 0, memory_used_kb: 0, created_at: 'x', finished_at: 'y' }, {});
    assert.equal(items2[0][1], 'id:7');
});

await test('buildMetaItems: \u8bed\u8a00\u4f18\u5148\u7528 langInfo.label', () => {
    const items = buildMetaItems({ username: 'a', user_id: 1, language: 'cpp',
        time_used_ms: 0, memory_used_kb: 0, created_at: '', finished_at: '' }, { label: 'C++' });
    assert.equal(items[1][1], 'C++');
});

await test('buildMetaItems: \u8bed\u8a00\u65e0 langInfo \u2192 fallback \u539f\u8bed\u8a00 id', () => {
    const items = buildMetaItems({ username: 'a', user_id: 1, language: 'python',
        time_used_ms: 0, memory_used_kb: 0, created_at: '', finished_at: '' }, {});
    assert.equal(items[1][1], 'python');
});

// ---- buildModalColumns -----------------------------------------------------
await test('buildModalColumns: \u6837\u4f8b\u70b9 \u2192 kind=sample, 3 \u5217', () => {
    const r = buildModalColumns({
        is_sample: true,
        input: '1 2\n', expected_output: '3\n', user_output: '4\n',
        status: 'WA',
    });
    assert.equal(r.kind, 'sample');
    assert.equal(r.columns.length, 3);
    assert.equal(r.columns[0].title, '\u8f93\u5165 Input');
    assert.equal(r.columns[0].text,  '1 2\n');
    assert.equal(r.columns[1].title, '\u9884\u671f\u8f93\u51fa');
    assert.equal(r.columns[1].text,  '3\n');
    assert.equal(r.columns[2].title, '\u4f60\u7684\u8f93\u51fa');
    assert.equal(r.columns[2].text,  '4\n');
});

await test('buildModalColumns: \u9690\u85cf\u70b9 \u2192 kind=hidden, hint', () => {
    const r = buildModalColumns({
        is_sample: false,
        input: 'SECRET', expected_output: 'SECRET', user_output: 'SECRET',
        status: 'WA',
    });
    assert.equal(r.kind, 'hidden');
    assert.equal(r.columns.length, 0);
    assert.match(r.hint, /\u4e0d\u5c55\u793a\u9690\u85cf\u70b9\u8be6\u60c5/);
});

await test('buildModalColumns: null / \u7f3a\u5b57\u6bb5\u2192\u7a7a\u6587\u672c', () => {
    const r = buildModalColumns({ is_sample: true, status: 'WA' });
    assert.equal(r.kind, 'sample');
    for (const col of r.columns) {
        assert.equal(col.text, '');
    }
});

// ---- splitLines ------------------------------------------------------------
await test('splitLines: \u666e\u901a\u4e09\u884c', () => {
    assert.deepEqual(splitLines('a\nb\nc'), ['a', 'b', 'c']);
});

await test('splitLines: \u5c3e\u90e8\u6362\u884c\u4e0d\u4ea7\u751f\u7a7a\u884c', () => {
    assert.deepEqual(splitLines('a\nb\n'), ['a', 'b']);
});

await test('splitLines: \u7a7a\u5b57\u7b26\u4e32 / null / undefined \u2192 []', () => {
    assert.deepEqual(splitLines(''),        []);
    assert.deepEqual(splitLines(null),      []);
    assert.deepEqual(splitLines(undefined), []);
});

await test('splitLines: \\r\\n \u4ea4\u53c9\u6362\u884c\u540c\u6837\u5904\u7406', () => {
    assert.deepEqual(splitLines('a\r\nb'), ['a', 'b']);
});

// ---- computeLineDiff -------------------------------------------------------
await test('computeLineDiff: \u5b8c\u5168\u4e00\u81f4 \u2192 \u5168 same', () => {
    const d = computeLineDiff('a\nb\nc', 'a\nb\nc');
    assert.equal(d.length, 3);
    assert.ok(d.every(x => x.kind === 'same'));
    assert.deepEqual(d.map(x => x.text), ['a', 'b', 'c']);
});

await test('computeLineDiff: actual \u591a\u4e00\u884c \u2192 added \u591a\u4e00\u6761', () => {
    const d = computeLineDiff('a\nb', 'a\nb\nc');
    const kinds = d.map(x => x.kind);
    const texts = d.map(x => x.text);
    assert.deepEqual(kinds, ['same', 'same', 'added']);
    assert.deepEqual(texts, ['a', 'b', 'c']);
});

await test('computeLineDiff: actual \u5c11\u4e00\u884c \u2192 removed \u4e00\u6761', () => {
    const d = computeLineDiff('a\nb\nc', 'a\nc');
    const kinds = d.map(x => x.kind);
    assert.deepEqual(kinds, ['same', 'removed', 'same']);
});

await test('computeLineDiff: \u5b8c\u5168\u4e0d\u540c \u2192 \u5168\u662f added/removed', () => {
    const d = computeLineDiff('x\ny', 'p\nq');
    // 排序 / 数量 / kind
    const added = d.filter(x => x.kind === 'added').map(x => x.text).sort();
    const removed = d.filter(x => x.kind === 'removed').map(x => x.text).sort();
    assert.deepEqual(added,   ['p', 'q']);
    assert.deepEqual(removed, ['x', 'y']);
});

await test('computeLineDiff: expected \u4e3a\u7a7a / null \u2192 \u5168 added', () => {
    const d = computeLineDiff('', 'a\nb');
    assert.deepEqual(d.map(x => x.kind), ['added', 'added']);
});

await test('computeLineDiff: actual \u4e3a\u7a7a / null \u2192 \u5168 removed', () => {
    const d = computeLineDiff('a\nb', '');
    assert.deepEqual(d.map(x => x.kind), ['removed', 'removed']);
});

await test('computeLineDiff: \u4e24\u8fb9\u90fd\u4e3a\u7a7a \u2192 []', () => {
    assert.deepEqual(computeLineDiff('', ''), []);
    assert.deepEqual(computeLineDiff(null, null), []);
});

await test('computeLineDiff: \u591a\u884c / \u957f\u6587\u672c\u4e0d\u5d29', () => {
    const e = Array.from({ length: 50 }, (_, i) => 'E' + i).join('\n');
    const a = Array.from({ length: 50 }, (_, i) => 'A' + i).join('\n');
    const d = computeLineDiff(e, a);
    assert.equal(d.length, 100);
    assert.equal(d.filter(x => x.kind === 'removed').length, 50);
    assert.equal(d.filter(x => x.kind === 'added').length, 50);
});

await test('computeLineDiff: \u5d4c\u5957\u5dee\u5f02\uff08WA \u5e38\u89c1\uff09', () => {
    // expected = "1\n2\n3", actual = "1\n2\n3\n" \u2192 \u591a\u4e00\u4e2a\u7a7a\u884c\uff08\u672b\u5c3e\u6362\u884c\uff09
    // splitLines \u4f1a\u53bb\u6389\u5c3e\u90e8\u7a7a\u884c\uff0c\u6240\u4ee5\u5b8c\u5168\u4e00\u81f4
    const d = computeLineDiff('1\n2\n3\n', '1\n2\n3');
    assert.ok(d.every(x => x.kind === 'same'));
});

// ---- diffSummary -----------------------------------------------------------
await test('diffSummary: \u7edf\u8ba1 added/removed \u6570\u91cf', () => {
    const d = computeLineDiff('a\nb', 'a\nc\nd');
    const s = diffSummary(d);
    assert.equal(s.added,   2);
    assert.equal(s.removed, 1);
});

await test('diffSummary: \u5b8c\u5168\u4e00\u81f4 \u2192 0/0', () => {
    const d = computeLineDiff('a', 'a');
    assert.deepEqual(diffSummary(d), { added: 0, removed: 0 });
});

// =============================================================================
//  AC-19 \u9a8c\u6536\u70b9\uff1a\u9519\u70b9\u8be6\u60c5\u9875\u5c55\u793a user_output\uff08\u4ec5 is_sample=1\uff09/expected/diff
// =============================================================================

await test('AC-19: \u6837\u4f8b WA \u70b9\u2192buildModalColumns \u8fd4\u4e09\u5217\uff08input/expected/user_output\uff09', () => {
    const r = buildModalColumns({
        is_sample: true,
        input: '1 2', expected_output: '3', user_output: '4',
        status: 'WA',
    });
    assert.equal(r.kind, 'sample');
    const titles = r.columns.map(c => c.title);
    assert.ok(titles.some(t => t.includes('\u8f93\u5165')));
    assert.ok(titles.some(t => t.includes('\u9884\u671f')));
    assert.ok(titles.some(t => t.includes('\u4f60\u7684')));
    // diff\uff1a\u8ba1\u7b97 expected vs actual
    const diff = computeLineDiff(r.columns[1].text, r.columns[2].text);
    assert.equal(diff.filter(d => d.kind === 'same').length, 0); // "3" vs "4" \u5b8c\u5168\u4e0d\u540c
});

await test('AC-19: \u6837\u4f8b AC \u70b9\u2192shouldShowViewButton=false\uff08\u4e0d\u9700\u5c55\u793a\u5dee\u5f02\uff09', () => {
    assert.equal(shouldShowViewButton({ status: 'AC', is_sample: true }), false);
});

await test('AC-19: \u9690\u85cf WA \u70b9\u2192kind=hidden\uff0c\u4ec5 hint\uff08\u4fdd\u62a4\u9898\u76ee\uff09', () => {
    const r = buildModalColumns({
        is_sample: false,
        input: 'SECRET', expected_output: 'SECRET', user_output: 'SECRET',
        status: 'WA',
    });
    assert.equal(r.kind, 'hidden');
    assert.equal(r.columns.length, 0);
    assert.match(r.hint, /\u4fdd\u62a4/);
});

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail > 0 ? 1 : 0);