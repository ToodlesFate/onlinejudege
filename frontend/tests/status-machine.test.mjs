// =============================================================================
//  tests/status-machine.test.mjs — components/status-machine.js 单元测试
//  (Node, no framework)
//
//  覆盖 SPEC §2.3.2 状态机 + §3.3.5 K 可视化：
//    1) MAIN_STEPS 顺序与 SPEC 文字一致
//    2) TERMINAL_RESULTS 8 态全集
//    3) computeStepStates：每种 status (+ result) 下的三节点状态
//    4) computeProgressPct：百分比对应 0/50/75/100/66
//    5) 早退出（CE/SE）→ running 标 skipped
//    6) 纯函数：相同输入永远相同输出
//    7) RESULT_HINT 覆盖全部 8 态
//    8) DOM 渲染（用 jsdom-lite 桩件）
// =============================================================================

import assert from 'node:assert/strict';

import {
    MAIN_STEPS,
    TERMINAL_RESULTS,
    RESULT_HINT,
    computeStepStates,
    computeProgressPct,
} from '../js/components/status-machine.js';

// ---------------------------------------------------------------------------
//  Mini jsdom-lite —— 让 createEl 在 Node 下能跑
//  完全模拟 document / HTMLElement 的最小子集
// ---------------------------------------------------------------------------
function makeDom() {
    /** @type {any} */
    const noopHandler = () => {};
    class FakeNode {
        constructor(tag) {
            this.tagName   = (tag || '').toUpperCase();
            this.children  = [];
            this.attrs     = {};
            this._listeners = {};
            this.style     = {};
            this._classes  = [];
            // 关键：所有方法用箭头函数捕获节点引用，避免 `this` 漂移
            const self = this;
            // dataset 是 Proxy：el.dataset.foo 自动转成 el.getAttribute('data-foo')
            this.dataset = new Proxy({}, {
                get(_, key) {
                    const v = self.attrs['data-' + String(key)];
                    return v == null ? undefined : v;
                },
                set(_, key, v) {
                    if (v == null) delete self.attrs['data-' + String(key)];
                    else self.attrs['data-' + String(key)] = String(v);
                    return true;
                },
                deleteProperty(_, key) {
                    delete self.attrs['data-' + String(key)];
                    return true;
                },
            });
            // classList 同样访问 self._classes
            this.classList = {
                add(...cs)      { for (const c of cs) if (!self._classes.includes(c)) self._classes.push(c); },
                remove(...cs)   { self._classes = self._classes.filter(c => !cs.includes(c)); },
                toggle(c, on)   { on ? this.add(c) : this.remove(c); },
                contains(c)     { return self._classes.includes(c); },
            };
        }
        get className() { return this._classes.join(' '); }
        set className(v) {
            this._classes = String(v).split(/\s+/).filter(Boolean);
        }
        appendChild(c) { this.children.push(c); c.parentNode = this; return c; }
        removeChild(c) { this.children = this.children.filter(x => x !== c); c.parentNode = null; return c; }
        replaceChildren(...nodes) {
            this.children = [];
            for (const n of nodes) { this.appendChild(n); }
        }
        replaceWith(n) { if (this.parentNode) this.parentNode.replaceChild(n, this); }
        replaceChild(n, old) {
            const i = this.children.indexOf(old);
            if (i >= 0) { this.children[i] = n; n.parentNode = this; old.parentNode = null; }
        }
        setAttribute(k, v) { this.attrs[k] = String(v); }
        getAttribute(k)    { return this.attrs[k] == null ? null : this.attrs[k]; }
        removeAttribute(k) { delete this.attrs[k]; }
        addEventListener(name, fn) { (this._listeners[name] = this._listeners[name] || []).push(fn); }
        querySelector(sel)    { return querySelectorAllImpl(this, sel)[0] || null; }
        querySelectorAll(sel) { return querySelectorAllImpl(this, sel); }
        get firstElementChild() { for (const c of this.children) if (c.tagName) return c; return null; }
        get lastElementChild()  { for (let i = this.children.length - 1; i >= 0; --i) if (this.children[i].tagName) return this.children[i]; return null; }
        get textContent() {
            return this.children.map(c => c.tagName ? c.textContent : (c.value != null ? c.value : '')).join('');
        }
        set textContent(v) {
            this.children = [{ tagName: '', value: String(v), parentNode: this }];
        }
        get innerHTML()    { return this.children.map(c => c.tagName ? c.outerHTML : c.value || '').join(''); }
        set innerHTML(v)   { this.children = []; this.appendChild({ tagName: '', value: String(v), parentNode: this }); }
        querySelector(sel) { return querySelectorAllImpl(this, sel)[0] || null; }
        querySelectorAll(sel) { return querySelectorAllImpl(this, sel); }
        contains(n) {
            if (n === this) return true;
            for (const c of this.children) if (c.contains && c.contains(n)) return true;
            return false;
        }
        get isConnected() { return this.parentNode !== undefined && this.parentNode !== null; }
    }
    function querySelectorAllImpl(root, sel) {
        const out = [];
        // 多段选择器（带空格）→ 后代选择器：找匹配最后一段的节点，验证祖先链
        if (sel.includes(' ')) {
            const parts = sel.trim().split(/\s+/);
            const visit = (n, depth) => {
                if (!n || !n.children) return;
                for (const c of n.children) {
                    if (!c.tagName) continue;
                    if (matchCompound(c, parts, depth)) out.push(c);
                    visit(c, depth + 1);
                }
            };
            visit(root, 0);
            return out;
        }
        // 单段
        const visit = (n) => {
            if (!n || !n.children) return;
            for (const c of n.children) {
                if (!c.tagName) continue;
                if (matchOne(c, sel)) out.push(c);
                visit(c);
            }
        };
        visit(root);
        return out;
    }

    function matchCompound(node, parts, depth) {
        // node 必须匹配 parts[parts.length-1]，且其祖先链（往上 depth 个）依次匹配剩余
        if (!matchOne(node, parts[parts.length - 1])) return false;
        // 检查祖先链
        let cur = node.parentNode;
        let p = parts.length - 2;
        while (cur && p >= 0) {
            if (cur.tagName && matchOne(cur, parts[p])) {
                p--;
            }
            cur = cur.parentNode;
        }
        return p < 0;
    }
    function matches(node, sel) {
        // 极简：支持 .cls、tag、[data-foo="bar"]、三者任意组合（无空格 → 单元素匹配）
        // 多个选择器用空格分隔时（后代选择器）由 querySelectorAll 拆开递归。
        if (!sel) return false;
        // 先按空格切成多段
        const parts = sel.trim().split(/\s+/);
        if (parts.length === 1) {
            return matchOne(node, parts[0]);
        }
        // 多段：要求 node 在层级里每段依次命中（不在这里做，用 querySelectorAll 的递归）
        return matchOne(node, parts[parts.length - 1]);
    }

    function matchOne(node, sel) {
        // 解析：tag? .class* [data-x="y"]* 任意组合
        let rest = sel;
        // 1) tag
        const tagM = rest.match(/^([a-zA-Z][a-zA-Z0-9_-]*)/);
        if (tagM) {
            if (node.tagName.toLowerCase() !== tagM[1].toLowerCase()) return false;
            rest = rest.slice(tagM[0].length);
        }
        // 2) 重复的 .class 与 [attr=val]
        let ok = tagM != null;   // 至少匹配了 tag
        while (rest.length > 0) {
            const cm = rest.match(/^\.([a-zA-Z0-9_-]+)/);
            if (cm) {
                if (!node.classList.contains(cm[1])) return false;
                rest = rest.slice(cm[0].length);
                ok = true;
                continue;
            }
            const am = rest.match(/^\[data-([a-zA-Z0-9_-]+)(?:="([^"]*)")?\]/);
            if (am) {
                const got = node.attrs['data-' + am[1]];
                if (am[2] != null) {
                    if (got !== am[2]) return false;
                } else {
                    if (got == null) return false;
                }
                rest = rest.slice(am[0].length);
                ok = true;
                continue;
            }
            break;
        }
        if (!ok && rest.length === 0 && !tagM) {
            // 空选择器
            return false;
        }
        return ok || rest.length === 0;
    }
    /** @type {any} */
    const document = {
        createElement: (tag) => new FakeNode(tag),
        // createTextNode 返回的节点 tagName 为空字符串、value 是文本；
        // FakeNode 的 textContent getter 通过 c.tagName 真值判断走 c.value。
        createTextNode: (t) => ({
            tagName: '',
            value: String(t),
            parentNode: null,
            get textContent() { return this.value; },
            set textContent(v) { this.value = String(v); },
        }),
    };
    /** @type {any} */
    const window = {};
    /** @type {any} */
    const globalScope = { document, window, Node: FakeNode };
    globalScope.global = globalScope;
    return globalScope;
}

// 装一个全局 document/window/Node —— createEl 在 Node 下读全局
const dom = makeDom();
globalThis.document = dom.document;
globalThis.window   = dom.window;
// 关键：让 dom.js 的 `instanceof Node` 用到 test 的 FakeNode；
// 否则两套 FakeNode 之间 instanceof 不命中 → span 被 toString 成 "[object Object]"
globalThis.Node    = dom.Node;

// 现在才 import createEl（它在 dom.js 里读 document.createElement）
const { createEl } = await import('../js/utils/dom.js');
const { createStatusMachine, updateStatusMachine } = await import('../js/components/status-machine.js');

// ---------------------------------------------------------------------------
let pass = 0, fail = 0;
function test(name, fn) {
    return Promise.resolve()
        .then(fn)
        .then(() => { pass++; console.log('  OK   ' + name); })
        .catch((e) => { fail++; console.log('  FAIL ' + name + '\n       ' + (e && e.stack || e)); });
}

(async () => {
    console.log('--- SPEC §2.3.2 / §3.3.5 K: 判题状态机 + 可视化 ---');

    // ============================================================ 静态数据
    await test('MAIN_STEPS 顺序 = queued → compiling → running', () => {
        assert.equal(MAIN_STEPS.length, 3);
        assert.deepEqual(MAIN_STEPS.map(s => s.id),
            ['queued', 'compiling', 'running']);
    });

    await test('TERMINAL_RESULTS 8 态 = AC/WA/TLE/MLE/OLE/RE/CE/SE', () => {
        assert.equal(TERMINAL_RESULTS.length, 8);
        assert.deepEqual(TERMINAL_RESULTS,
            ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE']);
    });

    await test('RESULT_HINT 覆盖 8 态', () => {
        for (const r of TERMINAL_RESULTS) {
            assert.ok(RESULT_HINT[r] && RESULT_HINT[r].length > 0,
                `${r} 应有释义`);
        }
    });

    // ============================================================ computeStepStates
    await test('computeStepStates: queued 节点 = active, 其余 pending', () => {
        assert.deepEqual(computeStepStates('queued'),
            { queued: 'active', compiling: 'pending', running: 'pending' });
    });

    await test('computeStepStates: compiling 节点', () => {
        assert.deepEqual(computeStepStates('compiling'),
            { queued: 'done', compiling: 'active', running: 'pending' });
    });

    await test('computeStepStates: running 节点', () => {
        assert.deepEqual(computeStepStates('running'),
            { queued: 'done', compiling: 'done', running: 'active' });
    });

    await test('computeStepStates: finished(AC/WA/TLE/MLE/OLE/RE) running=done', () => {
        for (const r of ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE']) {
            const s = computeStepStates('finished', r);
            assert.deepEqual(s,
                { queued: 'done', compiling: 'done', running: 'done' },
                `result=${r} 应全 done`);
        }
    });

    await test('computeStepStates: finished(CE/SE) running=skipped（早退出）', () => {
        for (const r of ['CE', 'SE']) {
            const s = computeStepStates('finished', r);
            assert.deepEqual(s,
                { queued: 'done', compiling: 'done', running: 'skipped' },
                `result=${r} 应早退出`);
        }
    });

    await test('computeStepStates: 未知 status → 全部 pending（防御）', () => {
        assert.deepEqual(computeStepStates('foo'),
            { queued: 'pending', compiling: 'pending', running: 'pending' });
    });

    // ============================================================ computeProgressPct
    await test('computeProgressPct: queued=0 / compiling=50 / running=75', () => {
        assert.equal(computeProgressPct('queued'),    0);
        assert.equal(computeProgressPct('compiling'), 50);
        assert.equal(computeProgressPct('running'),   75);
    });

    await test('computeProgressPct: finished(AC)=100 / finished(CE)=66', () => {
        assert.equal(computeProgressPct('finished', 'AC'), 100);
        assert.equal(computeProgressPct('finished', 'WA'), 100);
        assert.equal(computeProgressPct('finished', 'CE'),  66);
        assert.equal(computeProgressPct('finished', 'SE'),  66);
    });

    // ============================================================ DOM 渲染
    await test('createStatusMachine: 初始 = queued，渲染 3 节点 + 2 连线 + 进度条', () => {
        const el = createStatusMachine();
        assert.equal(el.classList.contains('sm'), true);
        assert.equal(el.dataset.status, 'queued');
        const nodes = el.querySelectorAll('.sm-node');
        const links = el.querySelectorAll('.sm-link');
        assert.equal(nodes.length, 3);
        assert.equal(links.length, 2);
        assert.ok(el.querySelector('.sm-progress__bar'));
        assert.ok(el.querySelector('.sm-footer'));
    });

    await test('updateStatusMachine: queued → compiling 改变 active 节点', () => {
        const el = createStatusMachine();
        updateStatusMachine(el, { status: 'compiling' });
        assert.equal(el.dataset.status, 'compiling');
        const active = el.querySelectorAll('.sm-node[data-state="active"]');
        const done   = el.querySelectorAll('.sm-node[data-state="done"]');
        assert.equal(active.length, 1);
        assert.equal(active[0].dataset.step, 'compiling');
        assert.equal(done.length, 1);
        assert.equal(done[0].dataset.step, 'queued');
    });

    await test('updateStatusMachine: finished(AC) → 全 done + result 区', () => {
        const el = createStatusMachine();
        updateStatusMachine(el, {
            status: 'finished', result: 'AC', total_score: 100,
            time_used_ms: 12, memory_used_kb: 4096,
        });
        assert.equal(el.dataset.status, 'finished');
        assert.equal(el.dataset.result, 'AC');
        const done = el.querySelectorAll('.sm-node[data-state="done"]');
        assert.equal(done.length, 3);
        const result = el.querySelector('.sm-footer__result');
        assert.ok(result, '应渲染结果区');
        assert.equal(result.textContent.includes('通过'), true, 'AC 显示"通过"');
    });

    await test('updateStatusMachine: finished(CE) → running=skipped + 编译错误提示', () => {
        const el = createStatusMachine();
        updateStatusMachine(el, { status: 'finished', result: 'CE' });
        const skipped = el.querySelectorAll('.sm-node[data-state="skipped"]');
        assert.equal(skipped.length, 1);
        assert.equal(skipped[0].dataset.step, 'running');
        const result = el.querySelector('.sm-footer__result');
        assert.equal(result.textContent.includes('编译错误'), true);
    });

    await test('updateStatusMachine: 进度条宽度随 status 改变', () => {
        const el = createStatusMachine();
        let bar = el.querySelector('.sm-progress__bar');
        assert.equal(bar.style.width, '0%');
        updateStatusMachine(el, { status: 'compiling' });
        bar = el.querySelector('.sm-progress__bar');
        assert.equal(bar.style.width, '50%');
        updateStatusMachine(el, { status: 'running' });
        bar = el.querySelector('.sm-progress__bar');
        assert.equal(bar.style.width, '75%');
        updateStatusMachine(el, { status: 'finished', result: 'AC' });
        bar = el.querySelector('.sm-progress__bar');
        assert.equal(bar.style.width, '100%');
        updateStatusMachine(el, { status: 'finished', result: 'CE' });
        bar = el.querySelector('.sm-progress__bar');
        assert.equal(bar.style.width, '66%');
    });

    await test('updateStatusMachine: 同输入 → 同输出（纯函数）', () => {
        const el1 = createStatusMachine();
        const el2 = createStatusMachine();
        updateStatusMachine(el1, { status: 'finished', result: 'WA' });
        updateStatusMachine(el2, { status: 'finished', result: 'WA' });
        // 状态机节点状态等价
        const ns1 = el1.querySelectorAll('.sm-node');
        const ns2 = el2.querySelectorAll('.sm-node');
        for (let i = 0; i < ns1.length; i++) {
            assert.equal(ns1[i].dataset.state, ns2[i].dataset.state);
        }
    });

    await test('updateStatusMachine: 终态后 footer 显示耗时 + 内存', () => {
        const el = createStatusMachine();
        updateStatusMachine(el, {
            status: 'finished', result: 'AC',
            time_used_ms: 1500, memory_used_kb: 12288,
        });
        const m = el.querySelector('.sm-footer__metrics');
        assert.ok(m, '应渲染 metrics');
        assert.equal(m.textContent.includes('1.50 s'), true);
        assert.equal(m.textContent.includes('12 MB'), true);
    });

    // ============================================================ 终态全集
    await test('8 态全部能渲染结果徽章（AC/WA/TLE/MLE/OLE/RE/CE/SE）', () => {
        for (const r of TERMINAL_RESULTS) {
            const el = createStatusMachine();
            updateStatusMachine(el, { status: 'finished', result: r });
            const r0 = el.querySelector('.sm-footer__result');
            assert.ok(r0, `result=${r} 应渲染结果区`);
            assert.ok(r0.textContent.length > 0);
        }
    });

    console.log('---');
    console.log(`Passed: ${pass}, Failed: ${fail}`);
    process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
