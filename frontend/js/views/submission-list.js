// =============================================================================
//  views/submission-list.js — 提交列表 (个人 / 公共) (SPEC §2.4 / §3.3.5 I)
//
//  两种 scope：
//    1) 我的提交 (scope=mine, 默认)
//       - /api/submissions (Bearer 鉴权)
//       - 可按 problem_id / language / status 过滤
//    2) 公共 AC 提交 (scope=public)
//       - /api/submissions/public (无鉴权)
//       - 仅展示 result=AC 的提交
//       - 多一列「用户」，少一列「操作」按钮（行整行可点击）
//
//  布局：
//    Header
//    过滤条：[题目▼] [语言▼] [状态▼]        共 N 条
//    表格：  ID │ 题目 │ (用户) │ 语言 │ 状态 │ 分数 │ 耗时 │ 内存 │ 时间 │ (操作)
//    分页
//
//  行为：
//    - 过滤条件变化 → debounce 300ms → 重查
//    - 表格行点击 → 跳 /submissions/:id
//    - 时间列：相对时间 + hover 显示绝对时间
//    - scope 通过 ?scope=public 切换；无 scope 时按 mine
//
//  URL 同步：?page=&problem_id=&language=&status=&scope=
//  鉴权：mine scope 需要登录；未登录跳 /login?redirect=...
// =============================================================================

import { createEl, loading, empty, errorBanner, pagination, statusBadge } from '../utils/dom.js';
import { list as apiList, listPublic as apiListPublic } from '../api/submissions.js';
import { authStore } from '../store/state.js';
import { navigate } from '../router.js';
import { formatTime, formatMemory, formatDateTime, relativeTime } from '../utils/format.js';
import { LANG_BY_ID } from '../components/monaco-loader.js';

const PAGE_SIZE = 20;
const DEBOUNCE_MS = 300;

const LANG_OPTIONS = [
    { v: '',         label: '全部语言' },
    { v: 'cpp',      label: 'C++' },
    { v: 'c',        label: 'C' },
    { v: 'java',     label: 'Java' },
    { v: 'python',   label: 'Python' },
    { v: 'go',       label: 'Go' },
];

// 8 态 + 4 状态枚举（status=queued/compiling/running/finished + result=AC/WA/...）
//   - mine scope：4 态 + 8 态 都能筛
//   - public scope：永远只 AC，所以这层 filter 在 UI 上仍展示但后端忽略 status
const STATUS_OPTIONS = [
    { v: '',          label: '全部状态' },
    { v: 'queued',    label: '排队中' },
    { v: 'compiling', label: '编译中' },
    { v: 'running',   label: '运行中' },
    { v: 'finished',  label: '已结束' },
    { v: 'AC',        label: '通过' },
    { v: 'WA',        label: '答案错误' },
    { v: 'TLE',       label: '超时' },
    { v: 'MLE',       label: '超内存' },
    { v: 'OLE',       label: '输出超限' },
    { v: 'RE',        label: '运行错误' },
    { v: 'CE',        label: '编译错误' },
    { v: 'SE',       label: '系统错误' },
];

export default async function submissionListView(_params, query) {
    // 解析 scope —— ?scope=public 切到公共视图
    const scope = query.get('scope') === 'public' ? 'public' : 'mine';

    // mine scope：未登录 → 跳登录
    if (scope === 'mine' && !authStore.isLoggedIn) {
        navigate('/login?redirect=' + encodeURIComponent('/submissions'));
        return createEl('div', { class: 'view container' });
    }

    const root = createEl('div', { class: 'view container sl-view' });

    const header = createEl('div', { class: 'view__header' }, [
        createEl('div', null, [
            createEl('h1', { class: 'view__title' },
                scope === 'public' ? '公共提交' : '我的提交'),
            createEl('p',  { class: 'view__subtitle' },
                scope === 'public'
                    ? '所有用户的 AC 通过记录，按时间倒序展示'
                    : '按时间倒序展示你的所有提交记录'),
        ]),
        createEl('div', null, [
            scope === 'public'
                ? createEl('a', {
                    class: 'btn btn--ghost btn--sm',
                    href: '/submissions',
                }, '← 我的提交')
                : createEl('a', {
                    class: 'btn btn--ghost btn--sm',
                    href: '/submissions?scope=public',
                }, '公共 AC 提交 →'),
        ]),
    ]);
    root.appendChild(header);

    const state = {
        scope,
        page:       parseInt(query.get('page') || '1', 10) || 1,
        size:       PAGE_SIZE,
        problem_id: query.get('problem_id') || '',
        language:   query.get('language') || '',
        status:     query.get('status') || '',
    };

    const toolbar = createEl('div', { class: 'pl-toolbar sl-toolbar' });
    root.appendChild(toolbar);

    const statusHost = createEl('div', { class: 'sl-status' });
    root.appendChild(statusHost);

    let lastResult = null;
    let debounceTimer = null;

    function renderToolbar() {
        toolbar.replaceChildren();

        // 题目 ID 搜索（数字输入）
        const prob = createEl('input', {
            class: 'form-input pl-search sl-prob',
            type: 'number',
            min: '1',
            placeholder: '题目 ID',
            value: state.problem_id || '',
            autocomplete: 'off',
        });
        prob.addEventListener('input', () => scheduleReload());
        toolbar.appendChild(prob);

        // 语言下拉
        const langSel = makeSelect(LANG_OPTIONS, state.language, (v) => {
            state.language = v;
            state.page = 1;
            syncUrl();
            runQuery();
        });
        toolbar.appendChild(langSel);

        // 状态下拉
        const statSel = makeSelect(STATUS_OPTIONS, state.status, (v) => {
            state.status = v;
            state.page = 1;
            syncUrl();
            runQuery();
        });
        toolbar.appendChild(statSel);

        // 重置按钮
        toolbar.appendChild(createEl('button', {
            class: 'btn btn--ghost btn--sm',
            type: 'button',
            onClick: () => {
                state.problem_id = '';
                state.language = '';
                state.status = '';
                state.page = 1;
                syncUrl();
                renderToolbar();
                runQuery();
            },
        }, '清除过滤'));
    }

    function makeSelect(options, value, onChange) {
        const sel = createEl('select', { class: 'form-select pl-select' });
        for (const o of options) {
            const opt = createEl('option', { value: o.v }, o.label);
            if (o.v === value) opt.selected = true;
            sel.appendChild(opt);
        }
        sel.addEventListener('change', () => onChange(sel.value));
        return sel;
    }

    function scheduleReload() {
        if (debounceTimer) clearTimeout(debounceTimer);
        debounceTimer = setTimeout(() => {
            state.problem_id = prob.value.trim();
            state.page = 1;
            syncUrl();
            runQuery();
        }, DEBOUNCE_MS);
    }

    function syncUrl() {
        const sp = new URLSearchParams();
        if (state.scope !== 'mine') sp.set('scope', state.scope);
        if (state.page > 1)        sp.set('page', String(state.page));
        if (state.problem_id)      sp.set('problem_id', state.problem_id);
        if (state.language)        sp.set('language', state.language);
        if (state.status)          sp.set('status', state.status);
        const qs = sp.toString();
        const url = '/submissions' + (qs ? '?' + qs : '');
        if (location.pathname + location.search !== url) {
            history.replaceState({}, '', url);
        }
    }

    async function runQuery() {
        statusHost.replaceChildren(loading('加载中...'));
        const q = {
            page: state.page,
            size: state.size,
        };
        if (state.problem_id) q.problem_id = state.problem_id;
        if (state.language)   q.language   = state.language;
        // status 仅在 mine scope 有意义（public 后端只返 AC）
        if (state.status && state.scope === 'mine') q.status = state.status;
        try {
            lastResult = (state.scope === 'public')
                ? await apiListPublic(q)
                : await apiList(q);
            renderTable();
        } catch (err) {
            statusHost.replaceChildren(errorBanner('加载失败：' + (err && err.message || err), {
                onRetry: runQuery,
            }));
        }
    }

    function renderTable() {
        statusHost.replaceChildren();
        if (!lastResult) return;
        const items = lastResult.items || [];
        if (!items.length) {
            statusHost.appendChild(empty({
                icon: '∅',
                title: '暂无提交',
                hint: state.scope === 'public'
                    ? '全站还没有 AC 提交记录'
                    : '调整过滤条件，或前往题库提交你的第一份代码',
                action: state.scope === 'public'
                    ? undefined
                    : { label: '去题库', href: '/problems' },
            }));
            return;
        }

        // 顶部计数条
        const total = lastResult.total || 0;
        const topBar = createEl('div', { class: 'sl-top muted' },
            state.scope === 'public'
                ? `共 ${total} 条 AC 通过记录`
                : `共 ${total} 条`);
        statusHost.appendChild(topBar);

        const table = createEl('table', { class: 'table sl-table' });
        const headers = state.scope === 'public'
            ? ['ID', '题目', '用户', '语言', '状态', '分数', '耗时', '内存', '时间']
            : ['ID', '题目', '语言', '状态', '分数', '耗时', '内存', '时间', '操作'];
        const thead = createEl('thead', null, [
            createEl('tr', null, headers.map(h => createEl('th', null, h))),
        ]);
        table.appendChild(thead);
        const tbody = createEl('tbody');
        for (const s of items) {
            const tr = createEl('tr', {
                onClick: () => navigate('/submissions/' + s.id),
                style: { cursor: 'pointer' },
            });
            tr.appendChild(createEl('td', null, `#${s.id}`));
            tr.appendChild(createEl('td', null, s.problem_title || `#${s.problem_id}`));
            if (state.scope === 'public') {
                tr.appendChild(createEl('td', null, s.username || `id:${s.user_id}`));
            }
            const L = LANG_BY_ID[s.language] || {};
            tr.appendChild(createEl('td', null, L.label || s.language));
            tr.appendChild(createEl('td', null, statusBadge(s.result || s.status)));
            tr.appendChild(createEl('td', null, `${s.total_score || 0}`));
            tr.appendChild(createEl('td', null, formatTime(s.time_used_ms)));
            tr.appendChild(createEl('td', null, formatMemory(s.memory_used_kb)));
            const tCell = createEl('td', null, relativeTime(s.created_at));
            tCell.title = formatDateTime(s.created_at);
            tr.appendChild(tCell);
            if (state.scope === 'mine') {
                tr.appendChild(createEl('td', null, [
                    createEl('a', {
                        href: '/submissions/' + s.id,
                        onClick: (e) => { e.stopPropagation(); },
                    }, '查看'),
                ]));
            }
            tbody.appendChild(tr);
        }
        table.appendChild(tbody);
        statusHost.appendChild(table);

        // 分页
        if (total > state.size) {
            statusHost.appendChild(pagination({
                page: state.page,
                size: state.size,
                total,
                onChange: (p) => {
                    state.page = p;
                    syncUrl();
                    runQuery();
                },
            }));
        }
    }

    renderToolbar();
    runQuery();
    return root;
}
