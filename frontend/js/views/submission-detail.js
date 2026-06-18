// =============================================================================
//  views/submission-detail.js — 提交详情页 (SPEC §2.4 / §3.3.5 K)
//
//  布局：
//    Header 状态条： 提交 #ID  [STATUS]  总分 X/100
//    元信息：      用户 / 语言 / 耗时 / 内存 / 提交时间 / 判完时间
//    状态机可视化： 进度条 + 3 节点 (queued/compiling/running) + 终态结果
//    Tabs：        [源代码] [测试点]
//    源代码区：    Monaco 只读模式（失败降级 textarea）
//    测试点表：    # / 状态 / 耗时 / 内存 / 分数 / 类型 / 详情
//    错点弹窗：    样例：input / expected / user_output（WA 时 user_output 高亮 diff）
//                 隐藏：仅显示 "为保护题目，不展示隐藏点详情"
//
//  行为：
//    - 进入页面：调 GET /api/submissions/:id
//      · status != 'finished' → 启动 2s 轮询（poller.js）
//      · status == 'finished' → 不轮询，直接渲染
//    - 终态 (finished)：渲染测试点表 + 结果区；CE 时显示 compile_output；SE 时显示 judge_message
//    - 错点"查看" → 弹模态（错点 = WA/TLE/MLE/OLE/RE，CE/SE 无测试点故无按钮）
//    - WA 时 user_output 列用 LCS 行级 diff 高亮（红 = 多出来 / 灰 = 一致）
//    - 离开页面：停止轮询 + 拆掉 Monaco
//
//  鉴权：
//    - 公开访问：仅当 result=AC；其他情况需登录且为本人（或 admin）
//    - 403 / 404 → toast + 回上一页
// =============================================================================

import { createEl, empty, errorBanner, statusBadge } from '../utils/dom.js';
import { get as apiGet } from '../api/submissions.js';
import { ApiError } from '../api/client.js';
import { createStatusMachine, updateStatusMachine } from '../components/status-machine.js';
import { createPoller, POLL_INTERVAL_MS, POLL_MAX_DURATION_MS } from '../utils/poller.js';
import { createEditorOrFallback, LANG_BY_ID } from '../components/monaco-loader.js';
import { navigate } from '../router.js';
import { toast } from '../components/toast.js';
import { formatDateTime, formatTime, formatMemory } from '../utils/format.js';
import {
    parseSubmissionId,
    pickStatusBadgeCode,
    formatTotalScore,
    formatCaseScore,
    emptyCasesMessage,
    buildMetaItems,
    buildModalColumns,
    shouldShowViewButton,
    computeLineDiff,
} from '../utils/submission-detail-helpers.js';

export default async function submissionDetailView(params /*, query */) {
    const id = parseSubmissionId(params);
    if (id == null) {
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
            const top   = root.querySelector('.sd-top');
            const meta  = root.querySelector('.sd-meta');
            const sm    = root.querySelector('.sm');
            const body  = root.querySelector('.sd-body');
            if (top)  top.replaceWith(renderTopBar(id, data));
            if (meta) meta.replaceWith(renderMeta(data));
            if (sm)   updateStatusMachine(sm, data);
            if (body) body.replaceWith(renderCases(data));
        },
        onFinish: () => {
            toast('判题完成', 'success');
        },
        onTimeout: () => {
            toast('判题超时（> 30 分钟），请稍后查看提交列表', 'warn');
        },
        onError: (err) => {
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
    bar.appendChild(createEl('span', { class: 'sd-top__id' }, `提交 #${id}`));
    if (detail) {
        bar.appendChild(statusBadge(pickStatusBadgeCode(detail)));
        bar.appendChild(createEl('span', { class: 'sd-top__score' },
            formatTotalScore(detail.total_score)));
    }
    return bar;
}

function renderMetaSkeleton() {
    return createEl('div', { class: 'sd-meta sd-meta--skeleton muted' }, '加载中…');
}

function renderMeta(detail) {
    const langInfo = LANG_BY_ID[detail.language] || { label: detail.language };
    const items = buildMetaItems({
        username:      detail.username,
        user_id:       detail.user_id,
        language:      detail.language,
        time_used_ms:  detail.time_used_ms,
        memory_used_kb:detail.memory_used_kb,
        created_at:    detail.created_at,
        finished_at:   detail.finished_at,
    }, langInfo);
    // 把每行的 value 翻译成展示文案（ms → "15 ms"、KB → "4 MB"、ISO → "2026-04-23 10:00:00"）
    const formatted = new Map([
        ['耗时',     formatTime(detail.time_used_ms)],
        ['内存',     formatMemory(detail.memory_used_kb)],
        ['提交时间', formatDateTime(detail.created_at)],
        ['判完时间', formatDateTime(detail.finished_at)],
    ]);

    const wrap = createEl('div', { class: 'sd-meta' });
    for (const [k, v] of items) {
        const cell = createEl('div', { class: 'sd-meta__cell' });
        cell.appendChild(createEl('div', { class: 'sd-meta__k muted' }, k));
        const display = formatted.has(k) ? formatted.get(k) : v;
        cell.appendChild(createEl('div', { class: 'sd-meta__v' }, display == null || display === '' ? '—' : String(display)));
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

    switchTab('cases');
    setTimeout(() => {
        const body = bodyHost.firstElementChild;
        if (body) body.classList.add('sd-body');
    }, 0);
    return wrap;
}

function renderCases(detail) {
    const cases = Array.isArray(detail.cases) ? detail.cases : [];

    if (!cases.length) {
        return createEl('div', { class: 'sd-cases sd-cases--empty muted' },
            emptyCasesMessage(detail));
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
        tr.appendChild(createEl('td', null, formatCaseScore(c.score)));
        tr.appendChild(createEl('td', null, c.is_sample ? '样例' : '隐藏'));
        // 详情列：错点 → 查看按钮；其余 → —
        const actTd = createEl('td');
        if (shouldShowViewButton(c)) {
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
//  错点弹窗 (SPEC §3.3.5 K)
//
//  - 样例点：3 列 (input / expected / user_output)
//    · WA 时，user_output 列内嵌 LCS 行级 diff 染色（红 = 多出 / 灰 = 一致）
//  - 隐藏点：仅显示 "为保护题目，不展示隐藏点详情"
// =============================================================================
function openCaseDialog(detail, c) {
    // 关闭已有
    document.querySelectorAll('.sd-modal-mask').forEach(n => n.remove());

    const modal = buildModalColumns(c);
    const isWa  = c.status === 'WA';

    const mask = createEl('div', { class: 'sd-modal-mask' });
    const dialog = createEl('div', { class: 'sd-modal' });
    const head  = createEl('div', { class: 'sd-modal__head' }, [
        createEl('h3', null, `测试点 #${c.case_index} · ${c.status}`),
        createEl('button', {
            class: 'btn btn--ghost btn--sm',
            type: 'button',
            onClick: () => mask.remove(),
        }, '关闭'),
    ]);
    dialog.appendChild(head);

    const body = createEl('div', { class: 'sd-modal__body' });
    if (modal.kind === 'hidden') {
        body.appendChild(createEl('div', { class: 'sd-modal__hint' }, modal.hint));
    } else {
        const grid = createEl('div', { class: 'sd-diff-grid' });
        // 三列
        grid.appendChild(makeDiffCol(modal.columns[0].title, modal.columns[0].text, false));
        grid.appendChild(makeDiffCol(modal.columns[1].title, modal.columns[1].text, false));
        // WA: 第三列用 diff 高亮（否则纯文本）
        if (isWa) {
            grid.appendChild(makeDiffColFromDiff(modal.columns[2].title, modal.columns[1].text, modal.columns[2].text));
        } else {
            grid.appendChild(makeDiffCol(modal.columns[2].title, modal.columns[2].text, true));
        }
        body.appendChild(grid);
        // diff 摘要
        if (isWa) {
            const diff = computeLineDiff(modal.columns[1].text, modal.columns[2].text);
            const same = diff.filter(d => d.kind === 'same').length;
            const total = diff.length;
            body.appendChild(createEl('div', { class: 'sd-diff-summary muted' },
                `行级 diff：共 ${total} 行，匹配 ${same} 行`));
        }
    }
    dialog.appendChild(body);
    mask.appendChild(dialog);
    mask.addEventListener('click', (e) => {
        if (e.target === mask) mask.remove();
    });
    document.body.appendChild(mask);
}

function makeDiffCol(title, text, isUserOutput) {
    return createEl('div', { class: 'sd-diff-col' }, [
        createEl('div', { class: 'sd-diff-col__title muted' }, title),
        createEl('pre', {
            class: 'sd-diff-col__pre' + (isUserOutput ? ' sd-diff-col__pre--user' : ''),
        }, text == null ? '' : String(text)),
    ]);
}

/**
 * 用 LCS 行级 diff 渲染 user_output。
 * 同 expected 的行标灰底（.sd-diff-line--same），
 * 多出来的行标红底（.sd-diff-line--added）。
 * @param {string} title
 * @param {string} expected
 * @param {string} actual
 */
function makeDiffColFromDiff(title, expected, actual) {
    const diffs = computeLineDiff(expected, actual);
    const pre = createEl('pre', { class: 'sd-diff-col__pre sd-diff-col__pre--user' });
    if (diffs.length === 0) {
        pre.appendChild(document.createTextNode(''));
    } else {
        diffs.forEach((d, i) => {
            const cls = d.kind === 'same'   ? 'sd-diff-line sd-diff-line--same'
                      : d.kind === 'added'  ? 'sd-diff-line sd-diff-line--added'
                      :                        'sd-diff-line sd-diff-line--removed';
            const line = createEl('span', { class: cls }, d.text);
            pre.appendChild(line);
            // 行分隔：除最后一行外补一个 \n
            if (i < diffs.length - 1) pre.appendChild(document.createTextNode('\n'));
        });
    }
    return createEl('div', { class: 'sd-diff-col' }, [
        createEl('div', { class: 'sd-diff-col__title muted' }, title),
        pre,
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