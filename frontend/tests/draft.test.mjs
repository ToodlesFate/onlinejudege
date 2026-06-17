// =============================================================================
//  tests/draft.test.mjs — utils/draft.js 单元测试 (Node, no framework)
//
//  覆盖 SPEC §3.3.5 H 的 3 条 + 4 个边缘 case：
//    1) load / save / clear 基本行为
//    2) key 格式 = "draft:problem_<id>:<lang>"
//    3) 空字符串 / null → removeItem（不留空白条目）
//    4) 多语言相互独立（同一 problem 不同 lang 互不干扰）
//    5) 容错：localStorage 抛异常 → load 返回空串 / save 返回 false，不抛
//    6) scheduler debounce：连续 schedule → 只触发一次 onSave
//    7) scheduler flush：手动 flush 立即触发并清 timer
//    8) scheduler cancel：cancel 后 flush 无效
// =============================================================================

import { makeDraftStore, DRAFT_PREFIX, DRAFT_DEBOUNCE_MS } from '../js/utils/draft.js';
import assert from 'node:assert/strict';

// ---------------------------------------------------------------------------
//  Mock localStorage —— 用 Map 模拟持久化 + 控制异常注入
// ---------------------------------------------------------------------------
function makeMockStorage() {
    const m = new Map();
    return {
        m,
        getItem: (k)    => m.has(k) ? m.get(k) : null,
        setItem: (k, v) => m.set(k, String(v)),
        removeItem: (k) => m.delete(k),
        clear:    ()    => m.clear(),
    };
}

function makeBrokenStorage() {
    return {
        getItem:    ()    => { throw new Error('SecurityError'); },
        setItem:    ()    => { throw new Error('QuotaExceeded'); },
        removeItem: ()    => { throw new Error('SecurityError'); },
    };
}

// ---------------------------------------------------------------------------
let pass = 0, fail = 0;
function test(name, fn) {
    return Promise.resolve()
        .then(fn)
        .then(() => { pass++; console.log('  OK   ' + name); })
        .catch((e) => { fail++; console.log('  FAIL ' + name + '\n       ' + (e && e.stack || e)); });
}

(async () => {
    console.log('--- SPEC §3.3.5 H: localStorage 草稿自动保存 ---');

    await test('key 格式 = "draft:problem_<id>:<lang>"', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        assert.equal(d.key(123, 'cpp'), 'draft:problem_123:cpp');
        assert.equal(d.key(7,  'python'), 'draft:problem_7:python');
        assert.equal(DRAFT_PREFIX, 'draft:problem_');
    });

    await test('save 后 load 取回原内容', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        const code = '#include <iostream>\nint main(){}\n';
        assert.equal(d.save(1, 'cpp', code), true);
        assert.equal(d.load(1, 'cpp'), code);
    });

    await test('load 缺失时返回空串（不返 null）', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        assert.equal(d.load(999, 'java'), '');
    });

    await test('空串 save 走 removeItem（不留空 key）', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        d.save(1, 'cpp', '#');
        assert.ok(s.m.has('draft:problem_1:cpp'));
        d.save(1, 'cpp', '');
        assert.equal(s.m.has('draft:problem_1:cpp'), false);
    });

    await test('clear 等价于 save(problemId, lang, "")', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        d.save(1, 'cpp', 'x');
        d.clear(1, 'cpp');
        assert.equal(d.load(1, 'cpp'), '');
        assert.equal(s.m.has('draft:problem_1:cpp'), false);
    });

    await test('多语言相互独立：cpp 的草稿不影响 java', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        d.save(1, 'cpp',   'C++ code');
        d.save(1, 'java',  'Java code');
        d.save(1, 'python','python code');
        assert.equal(d.load(1, 'cpp'),    'C++ code');
        assert.equal(d.load(1, 'java'),   'Java code');
        assert.equal(d.load(1, 'python'), 'python code');
        d.save(1, 'java', '');  // 只清 java
        assert.equal(d.load(1, 'cpp'),    'C++ code');
        assert.equal(d.load(1, 'java'),   '');
        assert.equal(d.load(1, 'python'), 'python code');
    });

    await test('多题目相互独立：problem 1 的草稿不影响 problem 2', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        d.save(1, 'cpp', 'P1');
        d.save(2, 'cpp', 'P2');
        assert.equal(d.load(1, 'cpp'), 'P1');
        assert.equal(d.load(2, 'cpp'), 'P2');
    });

    await test('容错：localStorage 抛异常时 load 返回空串（不抛）', () => {
        const d = makeDraftStore(makeBrokenStorage());
        assert.equal(d.load(1, 'cpp'), '');
    });

    await test('容错：localStorage 抛异常时 save 返回 false（不抛）', () => {
        const d = makeDraftStore(makeBrokenStorage());
        assert.equal(d.save(1, 'cpp', 'x'), false);
    });

    await test('容错：storage=null 时 load/save 静默返回默认值', () => {
        const d = makeDraftStore(null);
        assert.equal(d.load(1, 'cpp'), '');
        assert.equal(d.save(1, 'cpp', 'x'), false);
    });

    await test('scheduler debounce：500ms 内多次 schedule 只触发一次 onSave', async () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        let saves = 0;
        let lastCode = '';
        const sch = d.makeScheduler(
            () => lastCode,
            (pid, lang, code) => { saves++; d.save(pid, lang, code); },
            50,  // 加速测试
        );
        lastCode = 'a'; sch.schedule(1, 'cpp');
        lastCode = 'ab'; sch.schedule(1, 'cpp');
        lastCode = 'abc'; sch.schedule(1, 'cpp');
        await sleep(80);
        assert.equal(saves, 1, '只触发一次');
        assert.equal(d.load(1, 'cpp'), 'abc', '最终值是最后一次');
    });

    await test('scheduler flush 立即触发并清 timer', async () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        const calls = [];
        const sch = d.makeScheduler(
            () => 'v',
            (pid, lang, code) => { calls.push([pid, lang, code]); d.save(pid, lang, code); },
            1000,
        );
        sch.schedule(5, 'java');
        sch.flush();
        assert.equal(calls.length, 1);
        assert.deepEqual(calls[0], [5, 'java', 'v']);
        // flush 后再等原定时间 —— 不应再触发
        await sleep(1100);
        assert.equal(calls.length, 1);
    });

    await test('scheduler cancel 后 flush 无效', () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        let saves = 0;
        const sch = d.makeScheduler(
            () => 'v',
            () => { saves++; },
            1000,
        );
        sch.schedule(1, 'cpp');
        sch.cancel();
        sch.flush();
        assert.equal(saves, 0);
    });

    await test('scheduler 跨多次 schedule 用最后 (problemId, lang) 写', async () => {
        const s = makeMockStorage();
        const d = makeDraftStore(s);
        let lastArgs = null;
        const sch = d.makeScheduler(
            () => 'code',
            (pid, lang, code) => { lastArgs = { pid, lang, code }; },
            30,
        );
        sch.schedule(1, 'cpp');
        sch.schedule(1, 'java');  // 切到 java
        await sleep(60);
        assert.deepEqual(lastArgs, { pid: 1, lang: 'java', code: 'code' });
    });

    await test('debounce 常量 = 500ms（SPEC 规定）', () => {
        assert.equal(DRAFT_DEBOUNCE_MS, 500);
    });

    console.log('---');
    console.log(`Passed: ${pass}, Failed: ${fail}`);
    process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
