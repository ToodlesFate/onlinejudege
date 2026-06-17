// =============================================================================
//  views/problem-list.js — 题目列表页 (SPEC §3.3.5 G)
//
//  布局：
//    [Header]
//    搜索框 + 难度 + 标签(多选) + 排序
//    卡片网格（响应式 1/2/3 列）
//    分页器
//
//  行为：
//    - 过滤/排序变化 → debounce 300ms → 重新请求
//    - 卡片点击 → 跳 /problems/:id
//    - 标签 chip 点击 → 切换该 tag 的过滤选中态
//    - admin 卡片额外显示 "已通过 X / Y" 区域
//
//  URL 同步：
//    - 读 URL ?page=&difficulty=&tag=&sort=&q= 初始化
//    - 改过滤后写回 URL（replaceState）
//
//  状态：loading (8 骨架) / empty / error / data
// =============================================================================

import { createEl, loading, empty, errorBanner, pagination, difficultyBadge, tagChip } from '../utils/dom.js';
import { list as apiList, tags as apiTags } from '../api/problems.js';
import { authStore } from '../store/state.js';
import { formatPassRate } from '../utils/format.js';

// 困难度选项
const DIFFICULTIES = [
    { v: '',        label: '全部' },
    { v: 'easy',    label: '易' },
    { v: 'medium',  label: '中' },
    { v: 'hard',    label: '难' },
];

// 排序选项
const SORTS = [
    { v: 'created_desc',   label: '最新发布' },
    { v: 'id_desc',        label: '编号倒序' },
    { v: 'pass_rate_desc', label: '通过率高' },
];

const PAGE_SIZE = 20;
const DEBOUNCE_MS = 300;

export default async function problemListView(_params, query) {
    const root = createEl('div', { class: 'view container problem-list-view' });

    // ---- 顶部标题 ----
    const header = createEl('div', { class: 'view__header' }, [
        createEl('div', null, [
            createEl('h1', { class: 'view__title' }, '题库'),
            createEl('p',  { class: 'view__subtitle' }, '选择一道题目开始你的练习'),
        ]),
    ]);
    root.appendChild(header);

    // ---- 工具栏：搜索 + 过滤 + 排序 ----
    const toolbar = createEl('div', { class: 'pl-toolbar' });
    root.appendChild(toolbar);

    // 状态容器（loading / cards / empty / error）
    const status = createEl('div', { class: 'pl-status' });
    root.appendChild(status);

    // ---- 状态对象 ----
    const state = {
        page: 1,
        size: PAGE_SIZE,
        difficulty: '',
        tags: [],
        sort: 'created_desc',
        q: '',
    };

    // 标签缓存
    let allTags = [];
    let allProblems = [];
    let total = 0;

    // ---- 工具栏渲染 ----
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
        const debouncedSearch = debounce(() => {
            state.q = search.value.trim();
            state.page = 1;
            syncUrl();
            runQuery();
        }, DEBOUNCE_MS);
        search.addEventListener('input', debouncedSearch);
        toolbar.appendChild(search);

        // 难度
        const diffSelect = createEl('select', { class: 'form-select pl-select' });
        for (const d of DIFFICULTIES) {
            const opt = createEl('option', { value: d.v }, d.label);
            if (d.v === state.difficulty) opt.selected = true;
            diffSelect.appendChild(opt);
        }
        diffSelect.addEventListener('change', () => {
            state.difficulty = diffSelect.value;
            state.page = 1;
            syncUrl();
            runQuery();
        });
        toolbar.appendChild(diffSelect);

        // 排序
        const sortSelect = createEl('select', { class: 'form-select pl-select' });
        for (const s of SORTS) {
            const opt = createEl('option', { value: s.v }, s.label);
            if (s.v === state.sort) opt.selected = true;
            sortSelect.appendChild(opt);
        }
        sortSelect.addEventListener('change', () => {
            state.sort = sortSelect.value;
            state.page = 1;
            syncUrl();
            runQuery();
        });
        toolbar.appendChild(sortSelect);

        // 标签多选
        const tagBar = createEl('div', { class: 'pl-tags' });
        tagBar.appendChild(createEl('span', { class: 'pl-tags__label muted' }, '标签:'));
        for (const t of allTags) {
            const active = state.tags.includes(t.slug);
            const chip = tagChip(t, {
                active,
                onClick: () => {
                    if (active) state.tags = state.tags.filter(s => s !== t.slug);
                    else        state.tags = [...state.tags, t.slug];
                    state.page = 1;
                    syncUrl();
                    renderToolbar();
                    runQuery();
                },
            });
            tagBar.appendChild(chip);
        }
        if (state.tags.length > 0) {
            const clearBtn = createEl('button', {
                class: 'btn btn--sm btn--ghost',
                type: 'button',
                onClick: () => {
                    state.tags = [];
                    state.page = 1;
                    syncUrl();
                    renderToolbar();
                    runQuery();
                },
            }, '清除过滤');
            tagBar.appendChild(clearBtn);
        }
        toolbar.appendChild(tagBar);
    }

    // ---- 卡片网格 ----
    function renderCards() {
        const grid = createEl('div', { class: 'pl-grid' });
        const isAdmin = authStore.isAdmin;
        for (const p of allProblems) {
            const card = createEl('a', {
                class: 'card card--hover pl-card',
                href: '/problems/' + p.id,
            });
            // 标题
            const head = createEl('div', { class: 'pl-card__head' }, [
                createEl('span', { class: 'pl-card__id muted' }, '#' + p.id),
                difficultyBadge(p.difficulty),
            ]);
            card.appendChild(head);
            card.appendChild(createEl('h3', { class: 'pl-card__title' }, p.title));

            // 标签
            if (Array.isArray(p.tags) && p.tags.length) {
                const tagRow = createEl('div', { class: 'pl-card__tags' });
                const visible = p.tags.slice(0, 3);
                for (const t of visible) tagRow.appendChild(createEl('span', { class: 'tag', title: t.slug }, t.name));
                if (p.tags.length > 3) tagRow.appendChild(createEl('span', { class: 'muted pl-card__more' }, `+${p.tags.length - 3}`));
                card.appendChild(tagRow);
            }

            // 底部：通过率 + （admin）已通过/总提交
            const foot = createEl('div', { class: 'pl-card__foot' });
            const stats = p.stats || { total: 0, accepted: 0, pass_rate: 0 };
            const pr = createEl('span', { class: 'pl-card__rate' }, [
                createEl('span', { class: 'pl-card__rate-num' }, formatPassRate(stats.pass_rate)),
                createEl('span', { class: 'pl-card__rate-label muted' }, '通过率'),
            ]);
            foot.appendChild(pr);
            if (isAdmin && stats.total != null) {
                foot.appendChild(createEl('span', { class: 'pl-card__admin muted' },
                    `已通过 ${stats.accepted ?? 0} / ${stats.total}`));
            }
            card.appendChild(foot);
            grid.appendChild(card);
        }
        return grid;
    }

    // ---- 骨架 ----
    function renderSkeleton() {
        const grid = createEl('div', { class: 'pl-grid' });
        for (let i = 0; i < 8; ++i) {
            grid.appendChild(createEl('div', { class: 'card pl-card pl-card--skeleton' }, [
                createEl('div', { class: 'pl-skel pl-skel--title' }),
                createEl('div', { class: 'pl-skel pl-skel--tag' }),
                createEl('div', { class: 'pl-skel pl-skel--rate' }),
            ]));
        }
        return grid;
    }

    // ---- 查询 ----
    async function runQuery() {
        setStatus(renderSkeleton());
        try {
            const data = await apiList({
                page: state.page,
                size: state.size,
                difficulty: state.difficulty,
                tags: state.tags,
                sort: state.sort,
                q: state.q,
            });
            allProblems = data.items || [];
            total = data.total || 0;
            render();
        } catch (err) {
            setStatus(errorBanner('加载题目失败：' + (err && err.message || err), {
                onRetry: runQuery,
            }));
        }
    }

    function render() {
        if (allProblems.length === 0) {
            setStatus(empty({
                icon: '∅',
                title: '没有符合条件的题目',
                hint: '尝试调整过滤条件或清除搜索',
                action: state.q || state.difficulty || state.tags.length
                    ? { label: '清除过滤', onClick: clearAll }
                    : undefined,
            }));
            return;
        }
        const wrap = createEl('div');
        wrap.appendChild(renderCards());
        if (total > state.size) {
            wrap.appendChild(pagination({
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
        setStatus(wrap);
    }

    function clearAll() {
        state.q = '';
        state.difficulty = '';
        state.tags = [];
        state.sort = 'created_desc';
        state.page = 1;
        syncUrl();
        renderToolbar();
        runQuery();
    }

    function setStatus(node) {
        status.replaceChildren(node);
    }

    // ---- URL 同步 ----
    function syncUrl() {
        const sp = new URLSearchParams();
        if (state.page > 1)         sp.set('page', String(state.page));
        if (state.difficulty)       sp.set('difficulty', state.difficulty);
        if (state.tags.length)      sp.set('tag', state.tags.join(','));
        if (state.sort !== 'created_desc') sp.set('sort', state.sort);
        if (state.q)                sp.set('q', state.q);
        const qs = sp.toString();
        const url = '/problems' + (qs ? '?' + qs : '');
        history.replaceState({}, '', url);
    }

    function loadFromQuery() {
        if (!query) return;
        const p = parseInt(query.get('page') || '1', 10);
        if (p >= 1) state.page = p;
        const d = query.get('difficulty');
        if (d) state.difficulty = d;
        const tagStr = query.get('tag');
        if (tagStr) state.tags = tagStr.split(',').map(s => s.trim()).filter(Boolean);
        const s = query.get('sort');
        if (s) state.sort = s;
        const q = query.get('q');
        if (q) state.q = q;
    }

    // ---- 启动 ----
    loadFromQuery();
    setStatus(loading('正在加载题目...'));

    try {
        allTags = await apiTags();
    } catch (e) {
        console.warn('[problem-list] tags load failed:', e && e.message);
        allTags = [];
    }
    renderToolbar();
    await runQuery();

    return root;
}

function debounce(fn, ms) {
    let t = null;
    return (...args) => {
        if (t) clearTimeout(t);
        t = setTimeout(() => fn(...args), ms);
    };
}
