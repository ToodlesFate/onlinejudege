// =============================================================================
//  views/submission-detail.js — 提交详情页 (SPEC §3.3.5 K)
//
//  布局：
//    Header 状态条： 提交 #ID  [STATUS]  总分 X/100
//    元信息：      用户 / 语言 / 耗时 / 内存 / 提交时间 / 判完时间
//    状态机可视化： 进度条 + 3 节点 (queued/compiling/running) + 终态结果
//    Tabs：        [源代码] [测试点]
//    源代码区：    Monaco 只读模式
//    测试点表：    # / 状态 / 耗时 / 内存 / 分数 / 详情
//    错点弹窗：    样例：input / expected / user_output
//                 隐藏：仅显示 "为保护题目，不展示隐藏点详情"
//
//  行为：
//    - 进入页面：调 GET /api/submissions/:id
//      · status != 'finished' → 启动 2s 轮询（poller.js）
//      · status == 'finished' → 不轮询，直接渲染
//    - 终态 (finished)：渲染测试点表 + 结果区；CE 时显示 compile_output；SE 时显示 judge_message
//    - 错点"查看" → 弹模态
//    - 离开页面：停止轮询（router 替换 view-root 节点时触发）
//
//  鉴权：
//    - 公开访问：仅当 result=AC；其他情况需登录且为本人（或 admin）
//    - 403 / 404 → toast + 回上一页
// =============================================================================

import { createEl, loading, empty, errorBanner, statusBadge } from '../utils/dom.js';
import { get as apiGet } from '../api/submissions.js';
import { ApiError, HttpError } from '../api/client.js';
import { createStatusMachine, updateStatusMachine } from '../components/status-machine.js';
import { createPoller, POLL_INTERVAL_MS, POLL_MAX_DURATION_MS } from '../utils/poller.js';
import { createEditorOrFallback, LANG_BY_ID } from '../components/monaco-loader.js';
import { authStore } from '../store/state.js';
import { navigate } from '../router.js';
import { toast } from '../components/toast.js';
import { formatDateTime, formatTime, formatMemory } from '../utils/format.js';

// 提前退场的原因（CE/SE）—— 测试点不展示
const EARLY_EXIT_RESULTS = new Set(['CE', 'SE']);

// 错点（需要展示 diff / user_output）—— SPEC §2.4
const FAIL_RESULTS = new Set(['WA', 'TLE', 'MLE', 'OLE', 'RE']);

export default async function submissionDetailView(params /*, query */) {
    const id = parseInt(params.id, 10);
    if (!Number.isFinite(id) || id <= 0) {
        return renderNotFound();
    }

    const root = createEl('div', { class: 'view container sd-view' });

    // 顶部骨架
    root.appendChild(renderTopBar(id));
    root.appendChild(renderMetaSkeleton());
    root.appendChild(createEl('div', { class: 'sd-sm-wrap' }, [
        createEl('div', { class: 'card' }, [createStatusMachine()]),
    ]));
    const tabsHost = createEl('div', { class: 'sd-tabs' });
    root.appendChild(tabsHost);

    // 清理钩子：离开页面时停轮询 + 拆掉 Monaco
    let poller = null;
    let monacoHandle = null;
    const cleanup = () => {
        if (poller) { try { poller.stop(); } catch {} poller = null; }
        if (monacoHandle && monacoHandle.dispose) { try { monacoHandle.dispose(); } catch {} monacoHandle = null; }
    };
    root._cleanup = cleanup;

    // 拉一次 detail
    /** @type {any} */
    let detail = null;
    try {
        detail = await apiGet(id);
    } catch (err) {
        cleanup();
        root.replaceChildren(renderLoadError(id, err));
        return root;
    }
    if (!detail) {
        cleanup();
        root.replaceChildren(renderNotFound());
        return root;
    }

    // 渲染主体
    root.replaceChildren();
    root.appendChild(renderTopBar(id, detail));
    root.appendChild(renderMeta(detail));
    root.appendChild(renderStatusMachineCard(detail));
    root.appendChild(renderTabsAndBody(detail, {
        onCodeMount: (h) => { monacoHandle = h; },
    }));

    // 终态：不需要轮询
    if (detail.status === 'finished') {
        return root;
    }

    // 否则启动 2s 轮询（SPEC §2.3.4）
    poller = createPoller({
        fetcher: () => apiGet(id),
        intervalMs: POLL_INTERVAL_MS,
        maxDurationMs: POLL_MAX_DURATION_MS,
        shouldStop: (d) => !!(d && d.status === 'finished'),
        onTick: (data) => {
            if (!root.isConnected) return;
            // 更新头部 / 状态机 / 测试点表
            const top   = root.querySelector('.sd-top');
            const meta  = root.querySelector('.sd-meta');
            const sm    = root.querySelector('.sm');
            const body  = root.querySelector('.sd-body');
            if (top)  top.replaceWith(renderTopBar(id, data));
            if (meta) meta.replaceWith(renderMeta(data));
            if (sm)   updateStatusMachine(sm, data);
            if (body) body.replaceWith(renderCases(data));
        },
        onFinish: (data) => {
            toast('判题完成', 'success');
        },
        onTimeout: () => {
            toast('判题超时（> 30 分钟），请稍后查看提交列表', 'warn');
        },
        onError: (err) => {
            // 静默：网络抖动不打扰用户；累计 > 3 次才警告
            const attempts = (poller && poller.getAttempt) ? poller.getAttempt() : 0;
            if (attempts > 0 && attempts % 10 === 0) {
                console.warn('[submission-detail] poller fetch failed x', attempts, err && err.message);
            }
        },
    });
    poller.start();

    return root;
}

// =============================================================================
//  顶部 / 元信息
// =============================================================================
function renderTopBar(id, detail) {
    const bar = createEl('div', { class: 'sd-top' });
    bar.appendChild(createEl('a', {
        class: 'btn btn--ghost btn--sm',
        href: '/submissions',
    }, '← 我的提交'));
    const idSpan = createEl('span', { class: 'sd-top__id' }, `提交 #${id}`);
    bar.appendChild(idSpan);
    if (detail) {
        bar.appendChild(statusBadge(detail.result || detail.status));
        const score = createEl('span', { class: 'sd-top__score' },
            `总分 ${detail.total_score || 0} / 100`);
        bar.appendChild(score);
    }
    return bar;
}

function renderMetaSkeleton() {
    return createEl('div', { class: 'sd-meta sd-meta--skeleton muted' }, '加载中…');
}

function renderMeta(detail) {
    const wrap = createEl('div', { class: 'sd-meta' });
    const langInfo = LANG_BY_ID[detail.language] || { label: detail.language };
    const items = [
        ['用户',     detail.username || `id:${detail.user_id}`],
        ['语言',     langInfo.label || detail.language],
        ['耗时',     formatTime(detail.time_used_ms)],
        ['内存',     formatMemory(detail.memory_used_kb)],
        ['提交时间', formatDateTime(detail.created_at)],
        ['判完时间', formatDateTime(detail.finished_at)],
    ];
    for (const [k, v] of items) {
        const cell = createEl('div', { class: 'sd-meta__cell' });
        cell.appendChild(createEl('div', { class: 'sd-meta__k muted' }, k));
        cell.appendChild(createEl('div', { class: 'sd-meta__v' }, v == null || v === '' ? '—' : String(v)));
        wrap.appendChild(cell);
    }
    return wrap;
}

function renderStatusMachineCard(detail) {
    const card = createEl('div', { class: 'card sd-sm-card' });
    const h3 = createEl('h3', { class: 'sd-h3' }, '判题状态机');
    card.appendChild(h3);
    const sm = createStatusMachine();
    updateStatusMachine(sm, detail);
    card.appendChild(sm);
    // CE / SE 附文
    if (detail.result === 'CE' && detail.compile_output) {
        const co = createEl('div', { class: 'sd-compile' });
        co.appendChild(createEl('div', { class: 'sd-compile__title muted' }, '编译输出'));
        co.appendChild(createEl('pre', { class: 'sd-compile__pre' }, detail.compile_output));
        card.appendChild(co);
    }
    if (detail.result === 'SE' && detail.judge_message) {
        const sm2 = createEl('div', { class: 'sd-judge-msg' });
        sm2.appendChild(createEl('div', { class: 'muted' }, '系统提示'));
        sm2.appendChild(createEl('div', null, detail.judge_message));
        card.appendChild(sm2);
    }
    return createEl('div', { class: 'sd-sm-wrap' }, [card]);
}

// =============================================================================
//  Tabs：源代码 / 测试点
// =============================================================================
function renderTabsAndBody(detail, hooks) {
    const wrap = createEl('div', { class: 'sd-tabs-wrap' });

    // tab 头
    const head = createEl('div', { class: 'sd-tab-head' });
    const tabs = [
        { id: 'cases', label: `测试点 (${(detail.cases || []).length})` },
        { id: 'code',  label: '源代码' },
    ];
    const tabBtns = tabs.map(t =>
        createEl('button', {
            class: 'sd-tab',
            type: 'button',
            'data-tab': t.id,
            onClick: () => switchTab(t.id),
        }, t.label)
    );
    tabBtns.forEach(b => head.appendChild(b));
    wrap.appendChild(head);

    // tab body
    const bodyHost = createEl('div', { class: 'sd-body-host' });
    wrap.appendChild(bodyHost);

    let current = 'cases';

    function switchTab(id) {
        current = id;
        for (const b of head.querySelectorAll('.sd-tab')) {
            b.classList.toggle('is-active', b.dataset.tab === id);
        }
        render();
    }

    function render() {
        bodyHost.replaceChildren();
        if (current === 'cases') {
            bodyHost.appendChild(renderCases(detail));
        } else {
            bodyHost.appendChild(renderCode(detail, hooks));
        }
    }

    // 初次渲染：默认 cases
    switchTab('cases');
    // body 元素名 = .sd-body（poller 用它做局部刷新）
    setTimeout(() => {
        const body = bodyHost.firstElementChild;
        if (body) body.classList.add('sd-body');
    }, 0);
    return wrap;
}

function renderCases(detail) {
    const cases = Array.isArray(detail.cases) ? detail.cases : [];

    if (!cases.length) {
        const isEarly = EARLY_EXIT_RESULTS.has(detail.result);
        const msg = isEarly
            ? (detail.result === 'CE'
                ? '编译失败，无测试点运行'
                : '系统错误，无测试点结果')
            : '判题中…测试点结果即将到达';
        return createEl('div', { class: 'sd-cases sd-cases--empty muted' }, msg);
    }

    const table = createEl('table', { class: 'table sd-cases' });
    const thead = createEl('thead', null, [
        createEl('tr', null, [
            createEl('th', null, '#'),
            createEl('th', null, '状态'),
            createEl('th', null, '耗时'),
            createEl('th', null, '内存'),
            createEl('th', null, '分数'),
            createEl('th', null, '类型'),
            createEl('th', null, '详情'),
        ]),
    ]);
    table.appendChild(thead);

    const tbody = createEl('tbody');
    for (const c of cases) {
        const tr = createEl('tr');
        tr.appendChild(createEl('td', null, `#${c.case_index}`));
        tr.appendChild(createEl('td', null, statusBadge(c.status)));
        tr.appendChild(createEl('td', null, formatTime(c.time_used_ms)));
        tr.appendChild(createEl('td', null, formatMemory(c.memory_used_kb)));
        tr.appendChild(createEl('td', null, `${c.score || 0} / 100`));
        tr.appendChild(createEl('td', null, c.is_sample ? '样例' : '隐藏'));
        // 详情列：错点 → 查看按钮；AC → —；其余 → —
        const actTd = createEl('td');
        const isFail = FAIL_RESULTS.has(c.status);
        if (isFail) {
            const btn = createEl('button', {
                class: 'btn btn--sm btn--secondary',
                type: 'button',
                onClick: () => openCaseDialog(detail, c),
            }, '查看');
            actTd.appendChild(btn);
        } else {
            actTd.appendChild(createEl('span', { class: 'muted' }, '—'));
        }
        tr.appendChild(actTd);
        tbody.appendChild(tr);
    }
    table.appendChild(tbody);
    return table;
}

function renderCode(detail, hooks) {
    const wrap = createEl('div', { class: 'sd-code' });
    const host = createEl('div', { class: 'sd-code__editor' });
    wrap.appendChild(host);

    const langInfo = LANG_BY_ID[detail.language] || {};
    const monacoLang = langInfo.monacoLang || 'plaintext';
    // Monaco 异步加载 —— 等待；失败降级 textarea
    createEditorOrFallback(host, {
        value: detail.code || '',
        language: monacoLang,
        readOnly: true,
        onChange: () => {},
    }).then((r) => {
        if (hooks && hooks.onCodeMount) hooks.onCodeMount(r);
    }).catch((err) => {
        host.textContent = '加载编辑器失败：' + (err && err.message || err);
    });
    return wrap;
}

// =============================================================================
//  错点弹窗
// =============================================================================
function openCaseDialog(detail, c) {
    // 先关闭已有的
    document.querySelectorAll('.sd-modal-mask').forEach(n => n.remove());

    const isSample = !!c.is_sample;
    const mask = createEl('div', { class: 'sd-modal-mask' });
    const modal = createEl('div', { class: 'sd-modal' });
    const head  = createEl('div', { class: 'sd-modal__head' }, [
        createEl('h3', null, `测试点 #${c.case_index} · ${c.status}`),
        createEl('button', {
            class: 'btn btn--ghost btn--sm',
            type: 'button',
            onClick: () => mask.remove(),
        }, '关闭'),
    ]);
    modal.appendChild(head);
    const body = createEl('div', { class: 'sd-modal__body' });

    if (isSample) {
        const grid = createEl('div', { class: 'sd-diff-grid' });
        grid.appendChild(makeDiffCol('输入 Input', c.input || ''));
        grid.appendChild(makeDiffCol('预期输出', c.expected_output || ''));
        grid.appendChild(makeDiffCol('你的输出', c.user_output || ''));
        body.appendChild(grid);
    } else {
        body.appendChild(createEl('div', { class: 'sd-modal__hint' },
            '为保护题目，不展示隐藏点详情'));
    }
    modal.appendChild(body);
    mask.appendChild(modal);
    mask.addEventListener('click', (e) => {
        if (e.target === mask) mask.remove();
    });
    document.body.appendChild(mask);
}

function makeDiffCol(title, text) {
    return createEl('div', { class: 'sd-diff-col' }, [
        createEl('div', { class: 'sd-diff-col__title muted' }, title),
        createEl('pre', { class: 'sd-diff-col__pre' }, text == null ? '' : String(text)),
    ]);
}

// =============================================================================
//  错误态 / 404
// =============================================================================
function renderLoadError(id, err) {
    if (err instanceof ApiError && err.code === 1004) {
        return renderNotFound();
    }
    if (err instanceof ApiError && err.code === 1003) {
        toast('无权查看此提交', 'error');
        setTimeout(() => navigate('/submissions', { replace: true }), 200);
        return createEl('div', { class: 'view container' });
    }
    return createEl('div', { class: 'view container' }, [
        errorBanner('加载提交失败：' + (err && err.message || err), {
            onRetry: () => navigate('/submissions/' + id, { replace: true }),
        }),
    ]);
}

function renderNotFound() {
    const root = createEl('div', { class: 'view container' });
    root.appendChild(empty({
        icon: '404',
        title: '提交不存在',
        hint: '该提交可能被删除',
        action: { label: '返回提交列表', href: '/submissions' },
    }));
    return root;
}
