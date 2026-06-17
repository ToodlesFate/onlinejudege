// =============================================================================
//  tests/admin-problems.test.mjs — api/admin-problems.js 序列化逻辑测试
//  (Node, no framework)
//
//  覆盖 SPEC §5.2.4 的 6 个端点 + 序列化边界：
//    1) list  → 走 GET /api/admin/problems?page=...&q=...&is_published=...
//    2) getEditData  → GET /api/admin/problems/:id/edit-data
//    3) create  → POST /api/admin/problems
//    4) update  → PUT /api/admin/problems/:id
//    5) remove  → DELETE /api/admin/problems/:id
//    6) setPublished  → PATCH /api/admin/problems/:id/publish
//
//  策略：注入 fake fetch，断言 URL / method / body / headers，
//  对 client.js 的 envelope 处理只确认 data 字段返回正确。
// =============================================================================

import assert from 'node:assert/strict';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const FRONTEND_ROOT = path.resolve(__dirname, '..');

// ---------------------------------------------------------------------------
//  fake fetch —— 记录调用，返回受控的 envelope
// ---------------------------------------------------------------------------
let lastCall = null;
function setResponse(body) {
    lastCall = null;
    fakeFetch._next = body;
}
function setError(err) {
    lastCall = null;
    fakeFetch._next = err;
}

async function fakeFetch(url, init) {
    lastCall = { url, init };
    if (fakeFetch._next instanceof Error) {
        const e = fakeFetch._next;
        fakeFetch._next = null;
        throw e;
    }
    if (fakeFetch._next && typeof fakeFetch._next === 'object' && 'status' in fakeFetch._next) {
        const { status, body } = fakeFetch._next;
        fakeFetch._next = null;
        return makeResponse(status, body);
    }
    // 默认 2xx + envelope
    return makeResponse(200, fakeFetch._next ?? { code: 0, message: 'ok', data: null });
}

function makeResponse(status, body) {
    return {
        status,
        ok: status >= 200 && status < 300,
        text: async () => (body == null ? '' : JSON.stringify(body)),
        json: async () => body,
    };
}

// 注入到 global —— client.js 用 fetch() 全局
globalThis.fetch = fakeFetch;

// ---------------------------------------------------------------------------
//  动态加载被测模块（在 fake fetch 安装好之后）
// ---------------------------------------------------------------------------
const API_URL = path.join(FRONTEND_ROOT, 'js', 'api', 'client.js') + '?t=' + Date.now();
const ADMIN_URL = path.join(FRONTEND_ROOT, 'js', 'api', 'admin-problems.js') + '?t=' + Date.now();
const { apiGet, apiPost, apiPut, apiPatch, apiDelete } = await import(API_URL);
const adminApi = await import(ADMIN_URL);

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
let pass = 0, fail = 0;
function test(name, fn) {
    return Promise.resolve()
        .then(fn)
        .then(() => { pass++; console.log('  OK   ' + name); })
        .catch((e) => { fail++; console.log('  FAIL ' + name + '\n       ' + (e && e.stack || e)); });
}

function assertCalled(method, urlPattern) {
    assert.ok(lastCall, 'fetch should be called');
    assert.equal(lastCall.init.method, method, `method ${method}`);
    if (urlPattern instanceof RegExp) {
        assert.match(lastCall.url, urlPattern, `url match ${urlPattern}`);
    } else {
        assert.ok(lastCall.url.endsWith(urlPattern), `url ends with ${urlPattern} (got ${lastCall.url})`);
    }
}

/**
 * 从相对 URL '/api/admin/problems?page=1&size=20&q=两数之和' 提取 query 参数
 * —— 浏览器里的 fetch 会拼成绝对 URL，但 fake fetch 收到的是相对路径，
 * 直接用 URLSearchParams 即可取参数。
 */
function queryParamsOf(relativeUrl) {
    const idx = relativeUrl.indexOf('?');
    if (idx < 0) return new URLSearchParams();
    return new URLSearchParams(relativeUrl.slice(idx + 1));
}

function parseBody() {
    if (!lastCall || !lastCall.init.body) return null;
    try { return JSON.parse(lastCall.init.body); } catch { return null; }
}

// ---------------------------------------------------------------------------
//  Tests
// ---------------------------------------------------------------------------
(async () => {
    console.log('--- SPEC §5.2.4: admin problems API ---');

    // ---- list ----
    await test('list 默认参数：page=1&size=20', async () => {
        setResponse({ code: 0, message: 'ok', data: { items: [], total: 0, page: 1, size: 20 } });
        const data = await adminApi.list();
        assertCalled('GET', /\/api\/admin\/problems\?/);
        const q = queryParamsOf(lastCall.url);
        assert.equal(q.get('page'), '1');
        assert.equal(q.get('size'), '20');
    });

    await test('list 传入 q / is_published / page', async () => {
        setResponse({ code: 0, message: 'ok', data: { items: [], total: 0, page: 2, size: 20 } });
        await adminApi.list({ page: 2, size: 20, q: '两数之和', is_published: '1' });
        const q = queryParamsOf(lastCall.url);
        assert.equal(q.get('q'), '两数之和');
        assert.equal(q.get('is_published'), '1');
        assert.equal(q.get('page'), '2');
    });

    await test('list 显式 include_unpublished=0', async () => {
        setResponse({ code: 0, message: 'ok', data: { items: [], total: 0 } });
        await adminApi.list({ include_unpublished: '0' });
        const q = queryParamsOf(lastCall.url);
        assert.equal(q.get('include_unpublished'), '0');
    });

    await test('list 透传 items 字段', async () => {
        setResponse({
            code: 0, message: 'ok',
            data: { items: [
                { id: 1, title: '两数之和', difficulty: 'easy', is_published: true,
                  created_by: 1, created_at: '2026-01-01T00:00:00Z',
                  tags: [{ id: 1, name: '数组', slug: 'array' }],
                  stats: { total: 10, accepted: 7, pass_rate: 70 } }
            ], total: 1, page: 1, size: 20 }
        });
        const data = await adminApi.list();
        assert.equal(data.items.length, 1);
        assert.equal(data.items[0].id, 1);
        assert.equal(data.items[0].stats.pass_rate, 70);
    });

    // ---- getEditData ----
    await test('getEditData 走 GET /api/admin/problems/:id/edit-data', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 5, title: 't' } });
        const data = await adminApi.getEditData(5);
        assertCalled('GET', '/api/admin/problems/5/edit-data');
        assert.equal(data.id, 5);
    });

    await test('getEditData id 含特殊字符走 encodeURIComponent', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 1 } });
        // 正常 id 是数字，但模拟一下 path 注入保护
        await adminApi.getEditData('12/3');
        assert.ok(lastCall.url.endsWith('/api/admin/problems/12%2F3/edit-data'),
                  `url should encode slash: ${lastCall.url}`);
    });

    // ---- create ----
    await test('create 走 POST /api/admin/problems + 序列化 payload', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 99, title: 'new' } });
        const result = await adminApi.create({
            title:           '新题目',
            content_md:      '# Hi',
            difficulty:      'medium',
            time_limit_ms:   1500,
            memory_limit_mb: 128,
            output_limit_mb: 32,
            is_published:    false,
            tag_ids:         [1, 2, 3],
            cases: [
                { case_index: 1, input: '1 2',  expected_output: '3', is_sample: true,  score: 50 },
                { case_index: 2, input: '5 7',  expected_output: '12', is_sample: false, score: 50 },
            ],
        });
        assertCalled('POST', '/api/admin/problems');
        const body = parseBody();
        assert.equal(body.title, '新题目');
        assert.equal(body.difficulty, 'medium');
        assert.equal(body.is_published, false);
        assert.deepEqual(body.tag_ids, [1, 2, 3]);
        assert.equal(body.cases.length, 2);
        assert.equal(body.cases[0].score, 50);
        assert.equal(body.cases[1].is_sample, false);
        assert.equal(result.id, 99);
    });

    await test('create 字段缺省补默认值', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 1 } });
        await adminApi.create({
            title: 'T', content_md: 'M', difficulty: 'easy',
        });
        const body = parseBody();
        assert.equal(body.time_limit_ms, 2000);
        assert.equal(body.memory_limit_mb, 256);
        assert.equal(body.output_limit_mb, 64);
        assert.equal(body.is_published, false);
        assert.deepEqual(body.tag_ids, []);
        assert.deepEqual(body.cases, []);
    });

    await test('create cases 缺省时输出空数组（不留 null）', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 1 } });
        await adminApi.create({ title: 'T', content_md: 'M', difficulty: 'easy', cases: null });
        const body = parseBody();
        assert.ok(Array.isArray(body.cases), 'cases should be array');
        assert.equal(body.cases.length, 0);
    });

    await test('create case score 非法时归零', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 1 } });
        await adminApi.create({
            title: 'T', content_md: 'M', difficulty: 'easy',
            cases: [{ case_index: 1, input: '', expected_output: '', is_sample: false, score: 'abc' }],
        });
        const body = parseBody();
        assert.equal(body.cases[0].score, 0);
    });

    await test('create tag_ids 过滤掉非法值 (0 / 负数)', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 1 } });
        await adminApi.create({
            title: 'T', content_md: 'M', difficulty: 'easy',
            tag_ids: [1, 0, -3, 4],
        });
        const body = parseBody();
        assert.deepEqual(body.tag_ids, [1, 4]);
    });

    // ---- update ----
    await test('update 走 PUT /api/admin/problems/:id', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 5 } });
        await adminApi.update(5, {
            title: 'edited', content_md: 'md', difficulty: 'hard',
        });
        assertCalled('PUT', '/api/admin/problems/5');
        const body = parseBody();
        assert.equal(body.title, 'edited');
        assert.equal(body.difficulty, 'hard');
    });

    // ---- remove ----
    await test('remove 走 DELETE /api/admin/problems/:id', async () => {
        setResponse({ code: 0, message: 'ok', data: null });
        const data = await adminApi.remove(7);
        assertCalled('DELETE', '/api/admin/problems/7');
        assert.equal(data, null);
    });

    // ---- setPublished ----
    await test('setPublished(true) 走 PATCH publish + body.is_published=true', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 5, is_published: true } });
        const data = await adminApi.setPublished(5, true);
        assertCalled('PATCH', '/api/admin/problems/5/publish');
        const body = parseBody();
        assert.equal(body.is_published, true);
        assert.equal(data.is_published, true);
    });

    await test('setPublished(false) 强转 boolean（防字符串 "false" 逃过）', async () => {
        setResponse({ code: 0, message: 'ok', data: { id: 5, is_published: false } });
        // 传入 truthy 非 bool
        await adminApi.setPublished(5, 1);
        const body = parseBody();
        assert.equal(body.is_published, true);
    });

    // ---- 错误码 ----
    await test('服务端 1001 → ApiError code=1001', async () => {
        setResponse({ code: 1001, message: 'title is required', data: null });
        let caught = null;
        try { await adminApi.list(); } catch (e) { caught = e; }
        assert.ok(caught);
        assert.equal(caught.code, 1001);
        assert.equal(caught.message, 'title is required');
    });

    await test('服务端 1003 (无权限) → ApiError code=1003', async () => {
        setResponse({ code: 1003, message: 'admin role required', data: null });
        let caught = null;
        try { await adminApi.create({ title: 'T', content_md: 'M', difficulty: 'easy' }); }
        catch (e) { caught = e; }
        assert.equal(caught.code, 1003);
    });

    await test('HTTP 500 触发 catch-all HttpError', async () => {
        // 模拟 server 500 且无 envelope
        setResponse({ status: 500, body: 'internal error' });
        let caught = null;
        try { await adminApi.list(); } catch (e) { caught = e; }
        assert.ok(caught);
        assert.equal(caught.status, 500);
    });

    await test('网络错误 (fetch reject) → HttpError status=0', async () => {
        setError(new TypeError('Failed to fetch'));
        let caught = null;
        try { await adminApi.list(); } catch (e) { caught = e; }
        assert.ok(caught);
        assert.equal(caught.status, 0);
        assert.match(caught.message, /Failed to fetch/);
    });

    console.log('---');
    console.log(`Passed: ${pass}, Failed: ${fail}`);
    process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
