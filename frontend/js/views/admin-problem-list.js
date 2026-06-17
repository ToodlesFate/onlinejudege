// =============================================================================
//  views/admin-problem-list.js — 后台题目管理 (SPEC §3.3.5 L)
//
//  布局：
//    Header
//    工具栏：搜索 / 状态过滤 / [新建题目]
//    表格：ID │ 标题 │ 难度 │ 标签 │ 状态 │ 通过率 │ 创建者 │ 创建时间 │ 操作
//    分页
//
//  行为：
//    - 过滤条件变化 → debounce 300ms → 重新请求
//    - 「编辑」 → 跳 /admin/problems/:id/edit
//    - 「上下架」 → 弹确认 → PATCH publish → 刷新
//    - 「删除」 → 弹确认 → DELETE → 刷新
//    - 表格行点击（操作列除外） → 跳编辑页
//
//  鉴权：需要 admin；非 admin 视图直接返回 403 提示
// =============================================================================

import { createEl, loading, empty, errorBanner, pagination, difficultyBadge, tagChip } from '../utils/dom.js';
import { list as apiList, setPublished as apiSetPublished, remove as apiRemove } from '../api/admin-problems.js';
import { authStore } from '../store/state.js';
import { navigate } from '../router.js';
import { toast } from '../components/toast.js';
import { ApiError, HttpError } from '../api/client.js';
import { formatDateTime, formatPassRate } from '../utils/format.js';

const PAGE_SIZE = 20;
const DEBOUNCE_MS = 300;

const STATUS_OPTIONS = [
    { v: '',       label: '全部' },
    { v: '1',      label: '已发布' },
    { v: '0',      label: '草稿' },
];

export default async function adminProblemListView(_params, query) {
    if (!authStore.isLoggedIn) {
        navigate('/login?redirect=' + encodeURIComponent('/admin/problems'));
        return createEl('div', { class: 'view container' });
    }
    if (!authStore.isAdmin) {
        return renderForbidden();
    }

    const root = createEl('div', { class: 'view container apl-view' });

    // ---- 头部 ----
    root.appendChild(createEl('div', { class: 'view__header' }, [
        createEl('div', null, [
            createEl('h1', { class: 'view__title' }, '后台 · 题目管理'),
            createEl('p',  { class: 'view__subtitle' },
                '创建、编辑、上下架题目；只有 admin 可见'),
        ]),
        createEl('div', { class: 'view__actions' }, [
            createEl('a', {
                class: 'btn btn--primary',
                href: '/admin/problems/new',
            }, '+ 新建题目'),
        ]),
    ]));

    // ---- 状态 ----
    const state = {
        page:         parseInt(query.get('page') || '1', 10) || 1,
        size:         PAGE_SIZE,
        q:            query.get('q') || '',
        is_published: query.get('is_published') || '',
    };

    // ---- 工具栏 ----
    const toolbar = createEl('div', { class: 'pl-toolbar apl-toolbar' });
    root.appendChild(toolbar);

    // ---- 列表 / 状态容器 ----
    const statusHost = createEl('div', { class: 'apl-status' });
    root.appendChild(statusHost);

    let lastResult = null;
    let debounceTimer = null;

    function renderToolbar() {
        toolbar.replaceChildren();

        // 搜索框
        const search = createEl('input', {
            class: 'form-input pl-search',
            type: 'search',
            placeholder: '搜索题目标题...',
            value: state.q,
            autocomplete: 'off',
        });
        search.addEventListener('input', () => {
            if (debounceTimer) clearTimeout(debounceTimer);
            debounceTimer = setTimeout(() => {
                state.q = search.value.trim();
                state.page = 1;
                syncUrl();
                runQuery();
            }, DEBOUNCE_MS);
        });
        toolbar.appendChild(search);

        // 状态过滤
        const sel = createEl('select', { class: 'form-select pl-select' });
        for (const o of STATUS_OPTIONS) {
            const opt = createEl('option', { value: o.v }, o.label);
            if (o.v === state.is_published) opt.selected = true;
            sel.appendChild(opt);
        }
        sel.addEventListener('change', () => {
            state.is_published = sel.value;
            state.page = 1;
            syncUrl();
            runQuery();
        });
        toolbar.appendChild(sel);

        // 重置
        toolbar.appendChild(createEl('button', {
            class: 'btn btn--ghost btn--sm',
            type: 'button',
            onClick: () => {
                state.q = '';
                state.is_published = '';
                state.page = 1;
                syncUrl();
                renderToolbar();
                runQuery();
            },
        }, '清除过滤'));
    }

    function syncUrl() {
        const sp = new URLSearchParams();
        if (state.page > 1)             sp.set('page', state.page);
        if (state.q)                    sp.set('q', state.q);
        if (state.is_published)         sp.set('is_published', state.is_published);
        const qs = sp.toString();
        const url = '/admin/problems' + (qs ? '?' + qs : '');
        if (location.pathname + location.search !== url) {
            history.replaceState({}, '', url);
        }
    }

    async function runQuery() {
        statusHost.replaceChildren(loading('加载中...'));
        /** @type {Record<string, any>} */
        const q = { page: state.page, size: state.size };
        if (state.q)            q.q = state.q;
        if (state.is_published) q.is_published = state.is_published;
        try {
            lastResult = await apiList(q);
            renderTable();
        } catch (err) {
            if (err instanceof ApiError && err.code === 1003) {
                renderForbidden();
                return;
            }
            statusHost.replaceChildren(errorBanner(
                '加载失败：' + mapErr(err),
                { onRetry: runQuery }
            ));
        }
    }

    function renderTable() {
        statusHost.replaceChildren();
        if (!lastResult) return;
        const items = lastResult.items || [];
        const total = lastResult.total || 0;

        if (!items.length) {
            statusHost.appendChild(empty({
                icon: '∅',
                title: '暂无题目',
                hint: '点击右上角「新建题目」开始录入',
                action: { label: '+ 新建题目', href: '/admin/problems/new' },
            }));
            return;
        }

        // 计数条
        statusHost.appendChild(createEl('div', { class: 'sl-top muted' }, `共 ${total} 条`));

        const table = createEl('table', { class: 'table apl-table' });
        const thead = createEl('thead', null,
            createEl('tr', null, [
                th('ID',         { width: '70px'  }),
                th('标题',       null),
                th('难度',       { width: '70px'  }),
                th('标签',       null),
                th('状态',       { width: '80px'  }),
                th('通过率',     { width: '90px'  }),
                th('创建者',     { width: '120px' }),
                th('创建时间',   { width: '160px' }),
                th('操作',       { width: '220px' }),
            ])
        );
        table.appendChild(thead);

        const tbody = createEl('tbody');
        for (const p of items) {
            tbody.appendChild(renderRow(p));
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
                    window.scrollTo({ top: 0, behavior: 'smooth' });
                },
            }));
        }
    }

    function renderRow(p) {
        const tr = createEl('tr', {
            class: 'apl-row',
            style: { cursor: 'pointer' },
            onClick: () => navigate('/admin/problems/' + p.id + '/edit'),
        });

        tr.appendChild(createEl('td', null, '#' + p.id));
        tr.appendChild(createEl('td', { class: 'apl-title' }, p.title));
        tr.appendChild(createEl('td', null, difficultyBadge(p.difficulty)));

        // 标签
        const tagTd = createEl('td', null);
        if (Array.isArray(p.tags) && p.tags.length) {
            const wrap = createEl('div', { class: 'apl-tags' });
            const visible = p.tags.slice(0, 3);
            for (const t of visible) {
                const chip = tagChip(t);
                chip.addEventListener('click', (e) => {
                    e.stopPropagation();
                    // 跳公开题库时预填过滤
                    navigate('/problems?tag=' + encodeURIComponent(t.slug));
                });
                wrap.appendChild(chip);
            }
            if (p.tags.length > 3) {
                wrap.appendChild(createEl('span', { class: 'muted' }, `+${p.tags.length - 3}`));
            }
            tagTd.appendChild(wrap);
        } else {
            tagTd.appendChild(createEl('span', { class: 'muted' }, '—'));
        }
        tr.appendChild(tagTd);

        // 状态
        const statusTd = createEl('td', null);
        const badge = p.is_published
            ? createEl('span', { class: 'badge apl-status apl-status--pub' }, '已发布')
            : createEl('span', { class: 'badge apl-status apl-status--draft' }, '草稿');
        statusTd.appendChild(badge);
        tr.appendChild(statusTd);

        // 通过率
        const stats = p.stats || { total: 0, accepted: 0, pass_rate: 0 };
        tr.appendChild(createEl('td', null, formatPassRate(stats.pass_rate)));

        // 创建者
        tr.appendChild(createEl('td', { class: 'muted' }, '#' + (p.created_by ?? '—')));

        // 时间
        const tCell = createEl('td', { class: 'muted' }, formatDateTime(p.created_at));
        tCell.title = p.created_at;
        tr.appendChild(tCell);

        // 操作列
        tr.appendChild(renderActions(p));

        return tr;
    }

    function renderActions(p) {
        const td = createEl('td', { class: 'apl-actions' });

        const editBtn = createEl('a', {
            class: 'btn btn--sm btn--secondary',
            href: '/admin/problems/' + p.id + '/edit',
            onClick: (e) => e.stopPropagation(),
        }, '编辑');
        td.appendChild(editBtn);

        const toggleBtn = createEl('button', {
            class: 'btn btn--sm ' + (p.is_published ? 'btn--ghost' : 'btn--primary'),
            type: 'button',
            onClick: (e) => { e.stopPropagation(); togglePublish(p); },
        }, p.is_published ? '下架' : '发布');
        td.appendChild(toggleBtn);

        const delBtn = createEl('button', {
            class: 'btn btn--sm btn--danger',
            type: 'button',
            onClick: (e) => { e.stopPropagation(); confirmDelete(p); },
        }, '删除');
        td.appendChild(delBtn);

        return td;
    }

    async function togglePublish(p) {
        const next = !p.is_published;
        const verb = next ? '发布' : '下架';
        if (!confirm(`确认${verb}题目 #${p.id} "${p.title}" ？`)) return;
        try {
            await apiSetPublished(p.id, next);
            toast(`已${verb}题目 #${p.id}`, 'success');
            runQuery();
        } catch (err) {
            toast(`${verb}失败：${mapErr(err)}`, 'error');
        }
    }

    async function confirmDelete(p) {
        if (!confirm(`确认软删除题目 #${p.id} "${p.title}" ？\n` +
                     `（软删除会将其设为草稿，不会从数据库中物理删除）`)) {
            return;
        }
        try {
            await apiRemove(p.id);
            toast(`已删除题目 #${p.id}`, 'success');
            runQuery();
        } catch (err) {
            toast(`删除失败：${mapErr(err)}`, 'error');
        }
    }

    // ---- 启动 ----
    renderToolbar();
    await runQuery();
    return root;
}

// =============================================================================
//  辅助
// =============================================================================
function th(label, style) {
    const el = createEl('th', null, label);
    if (style) Object.assign(el.style, style);
    return el;
}

function renderForbidden() {
    return createEl('div', { class: 'view container' }, [
        createEl('div', { class: 'card' }, [
            createEl('h2', null, '403 · 无权限'),
            createEl('p',  { class: 'muted' },
                '后台管理仅对管理员开放。'),
            createEl('a', { class: 'btn btn--primary mt-3', href: '/' }, '返回首页'),
        ]),
    ]);
}

function mapErr(err) {
    if (err instanceof ApiError) {
        if (err.code === 1002) return '请先登录';
        if (err.code === 1003) return '无权限';
        if (err.code === 1004) return '资源不存在';
        return err.message || `错误 (code=${err.code})`;
    }
    if (err instanceof HttpError) {
        if (err.status === 0)  return '网络错误';
        if (err.status >= 500) return '服务异常';
        return `HTTP ${err.status}`;
    }
    return (err && err.message) || String(err);
}
