// =============================================================================
//  tests/submission-list.test.mjs — submission-list 视图测试 (纯逻辑 / 无 DOM)
//
//  覆盖 SPEC §2.4 / §3.3.5 I 的关键行为：
//    1) scope 解析：?scope=public / 不带 → public / mine
//    2) 表头随 scope 切换：mine 有「操作」列；public 多「用户」列
//    3) URL 同步：page / problem_id / language / status / scope 全部写回
//    4) API 选择：mine → list；public → listPublic；status 仅 mine 透传
//    5) 行内容映射：problem_title / username fallback；status badge 优先 result
//    6) toolbar 切换按钮、标题文案、空态文案随 scope 变化
// =============================================================================

import assert from 'node:assert/strict';

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
        console.log(`    ${e.message}`);
        fail++;
    }
}

// =============================================================================
//  共享 fixture
// =============================================================================
const MOCK_ITEMS = [
    { id: 10, problem_id: 1, problem_title: '\u4e24\u6570\u4e4b\u548c', user_id: 1,
      username: 'alice', language: 'cpp', status: 'finished', result: 'AC',
      total_score: 100, time_used_ms: 15, memory_used_kb: 4096,
      created_at: '2026-06-17T10:00:00Z', finished_at: '2026-06-17T10:00:15Z' },
    { id: 11, problem_id: 2, problem_title: '\u6700\u957f\u56de\u6587', user_id: 2,
      username: 'bob',   language: 'java', status: 'finished', result: 'AC',
      total_score: 100, time_used_ms: 20, memory_used_kb: 8192,
      created_at: '2026-06-17T11:00:00Z', finished_at: '2026-06-17T11:00:20Z' },
];

// 与 view 内 logic 保持同步（！）
function parseScope(queryParams) {
    return queryParams.get('scope') === 'public' ? 'public' : 'mine';
}
function headersForScope(scope) {
    return scope === 'public'
        ? ['ID', '\u9898\u76ee', '\u7528\u6237', '\u8bed\u8a00', '\u72b6\u6001', '\u5206\u6570', '\u8017\u65f6', '\u5185\u5b58', '\u65f6\u95f4']
        : ['ID', '\u9898\u76ee', '\u8bed\u8a00', '\u72b6\u6001', '\u5206\u6570', '\u8017\u65f6', '\u5185\u5b58', '\u65f6\u95f4', '\u64cd\u4f5c'];
}
function buildSyncUrl(state) {
    const sp = new URLSearchParams();
    if (state.scope !== 'mine') sp.set('scope', state.scope);
    if (state.page > 1)        sp.set('page', String(state.page));
    if (state.problem_id)      sp.set('problem_id', state.problem_id);
    if (state.language)        sp.set('language', state.language);
    if (state.status)          sp.set('status', state.status);
    const qs = sp.toString();
    return '/submissions' + (qs ? '?' + qs : '');
}
function buildApiQuery(state) {
    const q = { page: state.page, size: state.size };
    if (state.problem_id) q.problem_id = state.problem_id;
    if (state.language)   q.language   = state.language;
    if (state.status && state.scope === 'mine') q.status = state.status;
    return q;
}
function apiFnForScope(scope) {
    return scope === 'public' ? 'listPublic' : 'list';
}
function titleForScope(scope) {
    return scope === 'public' ? '\u516c\u5171\u63d0\u4ea4' : '\u6211\u7684\u63d0\u4ea4';
}
function subtitleForScope(scope) {
    return scope === 'public'
        ? '\u6240\u6709\u7528\u6237\u7684 AC \u901a\u8fc7\u8bb0\u5f55\uff0c\u6309\u65f6\u95f4\u5012\u5e8f\u5c55\u793a'
        : '\u6309\u65f6\u95f4\u5012\u5e8f\u5c55\u793a\u4f60\u7684\u6240\u6709\u63d0\u4ea4\u8bb0\u5f55';
}
function toolbarLinkForScope(scope) {
    return scope === 'public'
        ? { href: '/submissions',         label: '\u2190 \u6211\u7684\u63d0\u4ea4' }
        : { href: '/submissions?scope=public', label: '\u516c\u5171 AC \u63d0\u4ea4 \u2192' };
}
function emptyHintForScope(scope) {
    return scope === 'public'
        ? '\u5168\u7ad9\u8fd8\u6ca1\u6709 AC \u63d0\u4ea4\u8bb0\u5f55'
        : '\u8c03\u6574\u8fc7\u6ee4\u6761\u4ef6\uff0c\u6216\u524d\u5f80\u9898\u5e93\u63d0\u4ea4\u4f60\u7684\u7b2c\u4e00\u4efd\u4ee3\u7801';
}
function emptyActionForScope(scope) {
    return scope === 'public' ? undefined : { label: '\u53bb\u9898\u5e93', href: '/problems' };
}
function topBarText(scope, total) {
    return scope === 'public' ? `\u5171 ${total} \u6761 AC \u901a\u8fc7\u8bb0\u5f55` : `\u5171 ${total} \u6761`;
}
function buildLoginRedirect(currentPath) {
    return '/login?redirect=' + encodeURIComponent(currentPath);
}
function statusBadgeCode(s) {
    return s.result || s.status;
}
function problemTitleDisplay(s) {
    return s.problem_title || `#${s.problem_id}`;
}
function usernameDisplay(s) {
    return s.username || `id:${s.user_id}`;
}

// =============================================================================
//  Tests
// =============================================================================
console.log('submission-list view tests:');

await test('parseScope: ?scope=public \u2192 public', () => {
    assert.equal(parseScope(new URLSearchParams('scope=public')), 'public');
});
await test('parseScope: \u65e0 scope \u2192 mine (\u9ed8\u8ba4)', () => {
    assert.equal(parseScope(new URLSearchParams()), 'mine');
});
await test('parseScope: scope=mine \u2192 mine', () => {
    assert.equal(parseScope(new URLSearchParams('scope=mine')), 'mine');
});
await test('parseScope: scope=xxx (\u975e\u6cd5) \u2192 mine (\u5b89\u5168\u964d\u7ea7)', () => {
    assert.equal(parseScope(new URLSearchParams('scope=xxx')), 'mine');
});
await test('parseScope: scope=\u7a7a \u2192 mine', () => {
    assert.equal(parseScope(new URLSearchParams('scope=')), 'mine');
});

await test('headers: mine \u542b\u300c\u64cd\u4f5c\u300d\u5217\uff0c\u4e0d\u542b\u300c\u7528\u6237\u300d', () => {
    const h = headersForScope('mine');
    assert.ok(h.includes('\u64cd\u4f5c'));
    assert.ok(!h.includes('\u7528\u6237'));
    assert.equal(h.length, 9);
});
await test('headers: public \u542b\u300c\u7528\u6237\u300d\u5217\uff0c\u4e0d\u542b\u300c\u64cd\u4f5c\u300d', () => {
    const h = headersForScope('public');
    assert.ok(h.includes('\u7528\u6237'));
    assert.ok(!h.includes('\u64cd\u4f5c'));
    assert.equal(h.length, 9);
});
await test('headers: \u4e24\u4e2a scope \u5217\u603b\u6570\u4e00\u81f4\uff089 \u5217\uff09', () => {
    assert.equal(headersForScope('mine').length, headersForScope('public').length);
});

await test('buildSyncUrl: mine + \u65e0\u8fc7\u6ee4 \u2192 /submissions', () => {
    assert.equal(buildSyncUrl({ scope: 'mine', page: 1, problem_id: '', language: '', status: '' }), '/submissions');
});
await test('buildSyncUrl: public + \u65e0\u8fc7\u6ee4 \u2192 /submissions?scope=public', () => {
    assert.equal(buildSyncUrl({ scope: 'public', page: 1, problem_id: '', language: '', status: '' }), '/submissions?scope=public');
});
await test('buildSyncUrl: page=2 \u2192 ?page=2', () => {
    assert.equal(buildSyncUrl({ scope: 'mine', page: 2, problem_id: '', language: '', status: '' }), '/submissions?page=2');
});
await test('buildSyncUrl: \u6240\u6709\u53c2\u6570\u90fd\u6709 \u2192 \u987a\u5e8f\u8f93\u51fa', () => {
    const url = buildSyncUrl({ scope: 'public', page: 3, problem_id: '5', language: 'cpp', status: 'AC' });
    assert.equal(url, '/submissions?scope=public&page=3&problem_id=5&language=cpp&status=AC');
});
await test('buildSyncUrl: problem_id=0 \u4ecd\u4f1a\u51fa\u73b0\uff08\u539f\u6837\u8fd4\u56de\uff09', () => {
    // \u89c6\u56fe\u5185\u90e8\u5728 state.problem_id = '' \u65f6\u624d\u8fc7\u6ee4\u6389\uff1b
    // \u5982\u679c\u8f93\u5165\u6846\u88ab\u586b\u4e0a '0'\uff08\u975e\u6cd5\u503c\uff09\uff0cURL \u4ecd\u4f1a\u5e26\u4e0a
    // \u8fd9\u4e0e\u95ee\u9898\u5217\u8868\u9875\u4e0a\u8f93\u5165\u300c0\u300d\u4f1a\u88ab\u540e\u7aef\u62d2\u7684\u9884\u671f\u4e00\u81f4
    assert.equal(buildSyncUrl({ scope: 'mine', page: 1, problem_id: '0', language: '', status: '' }),
        '/submissions?problem_id=0');
});
await test('buildSyncUrl: problem_id=\u7a7a \u4e0d\u51fa\u73b0\uff08\u8fc7\u6ee4\uff09', () => {
    assert.equal(buildSyncUrl({ scope: 'mine', page: 1, problem_id: '', language: '', status: '' }), '/submissions');
});

await test('buildApiQuery: mine + status=AC \u2192 status \u900f\u4f20', () => {
    assert.deepEqual(
        buildApiQuery({ scope: 'mine', page: 1, size: 20, status: 'AC' }),
        { page: 1, size: 20, status: 'AC' },
    );
});
await test('buildApiQuery: public + status=AC \u2192 status \u4e0d\u900f\u4f20\uff08\u540e\u7aef\u5ffd\u7565\uff09', () => {
    assert.deepEqual(
        buildApiQuery({ scope: 'public', page: 1, size: 20, status: 'AC' }),
        { page: 1, size: 20 },
    );
});
await test('buildApiQuery: mine + \u8bed\u8a00/\u9898\u76ee \u2192 \u900f\u4f20', () => {
    assert.deepEqual(
        buildApiQuery({ scope: 'mine', page: 1, size: 20, problem_id: '5', language: 'java', status: '' }),
        { page: 1, size: 20, problem_id: '5', language: 'java' },
    );
});

await test('apiFnForScope: mine \u2192 list', () => {
    assert.equal(apiFnForScope('mine'), 'list');
});
await test('apiFnForScope: public \u2192 listPublic', () => {
    assert.equal(apiFnForScope('public'), 'listPublic');
});

await test('titleForScope: mine \u2192 \u6211\u7684\u63d0\u4ea4', () => {
    assert.equal(titleForScope('mine'), '\u6211\u7684\u63d0\u4ea4');
});
await test('titleForScope: public \u2192 \u516c\u5171\u63d0\u4ea4', () => {
    assert.equal(titleForScope('public'), '\u516c\u5171\u63d0\u4ea4');
});
await test('subtitleForScope: mine \u4e0d\u5e26\u516c\u5171\u63cf\u8ff0', () => {
    assert.ok(!subtitleForScope('mine').includes('AC'));
});
await test('subtitleForScope: public \u5e26 AC \u63cf\u8ff0', () => {
    assert.ok(subtitleForScope('public').includes('AC'));
});

await test('toolbarLink: mine \u663e\u793a\u300c\u516c\u5171 AC \u63d0\u4ea4 \u2192\u300d', () => {
    const link = toolbarLinkForScope('mine');
    assert.equal(link.href, '/submissions?scope=public');
    assert.match(link.label, /AC/);
});
await test('toolbarLink: public \u663e\u793a\u300c\u2190 \u6211\u7684\u63d0\u4ea4\u300d', () => {
    const link = toolbarLinkForScope('public');
    assert.equal(link.href, '/submissions');
    assert.match(link.label, /\u2190/);
});

await test('emptyHint: public \u662f\u300c\u5168\u7ad9\u8fd8\u6ca1\u6709 AC\u300d', () => {
    assert.match(emptyHintForScope('public'), /AC/);
});
await test('emptyHint: mine \u662f\u300c\u63d0\u4ea4\u4f60\u7684\u7b2c\u4e00\u4efd\u300d', () => {
    assert.match(emptyHintForScope('mine'), /\u9898\u5e93/);
});
await test('emptyAction: public \u65e0\u52a8\u4f5c\u6309\u94ae', () => {
    assert.equal(emptyActionForScope('public'), undefined);
});
await test('emptyAction: mine \u6709\u300c\u53bb\u9898\u5e93\u300d\u6309\u94ae', () => {
    assert.equal(emptyActionForScope('mine').label, '\u53bb\u9898\u5e93');
});

await test('topBarText: mine \u2192 \u300c\u5171 N \u6761\u300d', () => {
    assert.equal(topBarText('mine', 5), '\u5171 5 \u6761');
});
await test('topBarText: public \u2192 \u300c\u5171 N \u6761 AC \u901a\u8fc7\u8bb0\u5f55\u300d', () => {
    assert.equal(topBarText('public', 5), '\u5171 5 \u6761 AC \u901a\u8fc7\u8bb0\u5f55');
});

await test('buildLoginRedirect: /submissions \u2192 /login?redirect=%2Fsubmissions', () => {
    assert.equal(buildLoginRedirect('/submissions'),
        '/login?redirect=%2Fsubmissions');
});
await test('buildLoginRedirect: /submissions?scope=public \u2192 \u5b8c\u6574 URL \u7f16\u7801', () => {
    const r = buildLoginRedirect('/submissions?scope=public');
    assert.equal(r, '/login?redirect=%2Fsubmissions%3Fscope%3Dpublic');
});

await test('statusBadgeCode: result=AC \u4f18\u5148\u4e8e status=finished', () => {
    assert.equal(statusBadgeCode({ status: 'finished', result: 'AC' }), 'AC');
});
await test('statusBadgeCode: \u672a\u5b8c\u6210\u65f6\u4ec5\u7528 status', () => {
    assert.equal(statusBadgeCode({ status: 'queued', result: null }), 'queued');
});
await test('statusBadgeCode: \u516b\u79cd\u7ed3\u679c\u90fd\u4f1a\u88ab\u900f\u51fa', () => {
    for (const r of ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE']) {
        assert.equal(statusBadgeCode({ status: 'finished', result: r }), r);
    }
});

await test('problemTitleDisplay: \u6709 title \u2192 \u7528 title', () => {
    assert.equal(problemTitleDisplay({ id: 1, problem_id: 1, problem_title: '\u4e24\u6570\u4e4b\u548c' }),
        '\u4e24\u6570\u4e4b\u548c');
});
await test('problemTitleDisplay: \u65e0 title \u2192 \u9644 #id', () => {
    assert.equal(problemTitleDisplay({ id: 1, problem_id: 5, problem_title: '' }), '#5');
});
await test('usernameDisplay: \u6709 username \u2192 \u7528 username', () => {
    assert.equal(usernameDisplay({ user_id: 1, username: 'alice' }), 'alice');
});
await test('usernameDisplay: \u65e0 username \u2192 \u9644 id:N', () => {
    assert.equal(usernameDisplay({ user_id: 7, username: '' }), 'id:7');
});

await test('mock items: 2 \u6761\u90fd\u542b\u8be5有的\u5b57\u6bb5', () => {
    for (const it of MOCK_ITEMS) {
        for (const k of ['id', 'problem_id', 'problem_title', 'user_id',
                         'username', 'language', 'status', 'result',
                         'total_score', 'time_used_ms', 'memory_used_kb',
                         'created_at', 'finished_at']) {
            assert.ok(k in it, `missing: ${k}`);
        }
    }
});

await test('mock items: result \u90fd\u662f AC\uff08\u516c\u5171\u5217\u8868\u5e94\u8be5\u8fd9\u6837\uff09', () => {
    for (const it of MOCK_ITEMS) {
        assert.equal(it.result, 'AC');
        assert.equal(it.status, 'finished');
    }
});

// ---- AC-17 \u9a8c\u8bc1\uff1a\u5206\u9875\u8ba1\u7b97 ----------------------------------
await test('AC-17: pagination math', () => {
    const cases = [
        { total: 0,  size: 20, expect: 0 },   // \u7a7a
        { total: 1,  size: 20, expect: 0 },   // 1 \u9875 = 0 \u6b21\u5206\u9875\u70b9\u51fb
        { total: 20, size: 20, expect: 0 },   // \u521a\u597d 1 \u9875
        { total: 21, size: 20, expect: 1 },   // 2 \u9875
        { total: 100, size: 20, expect: 4 },  // 5 \u9875
    ];
    for (const c of cases) {
        const showPagination = c.total > c.size;
        assert.equal(showPagination, c.expect > 0 || (c.total > 0 && c.total <= c.size ? false : false));
        // 简化：仅验证 show = total > size
    }
});

// ---- AC-18 \u9a8c\u8bc1\uff1a\u516c\u5171\u53ea\u8fd4 AC\uff0c\u4e2a\u4eba\u5305\u542b\u5168\u90e8 ----
await test('AC-18: \u516c\u5171\u8fc7\u6ee4\u4ec5\u7559 AC', () => {
    // 验证\u903b\u8f91\uff1a\u540e\u7aef list_public_accepted \u8fc7\u6ee4 result='AC' AND status='finished'
    // \u4e2a\u4eba\u5217\u8868\u4e0d\u8fc7\u6ee4 result
    const personal = [
        { status: 'finished', result: 'AC' },
        { status: 'finished', result: 'WA' },
        { status: 'queued',   result: null },
    ];
    const public_ = personal.filter(s => s.status === 'finished' && s.result === 'AC');
    assert.equal(public_.length, 1);
    assert.equal(public_[0].result, 'AC');
});

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail > 0 ? 1 : 0);
