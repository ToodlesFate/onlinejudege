// =============================================================================
//  views/problem-detail.js — 题目详情页 (SPEC §3.3.5 H)
//
//  布局：两栏（左 50% 题面 / 右 50% 编辑器），窄屏堆叠
//  左栏：标题 / 难度 / 标签 / Markdown 题面 / 样例 / 时空限制
//  右栏：语言选择 / Monaco / 重置 / 提交
//
//  行为：
//    - 进入：从 localStorage 恢复 draft:problem_<id>:<lang>
//    - 编辑器变化：debounce 500ms 写回 localStorage
//    - 切换语言：保存当前到旧 lang key，从新 lang key 恢复
//    - 提交：弹模态确认 → POST /api/submissions → 跳 /submissions/:id
//    - 题面 404：空状态 + 回列表按钮
//    - Monaco 加载失败：降级 textarea
//    - 未登录访问 → 提交时 Toast 提示跳登录（点击提交才检查）
//
//  API：
//    GET  /api/problems/:id   → 详情
//    POST /api/submissions    body {problem_id, language, code} → {submission_id}
// =============================================================================

import { createEl, loading, empty, errorBanner, difficultyBadge, tagChip } from '../utils/dom.js';
import { get as apiGet } from '../api/problems.js';
import { apiPost, ApiError, HttpError } from '../api/client.js';
import { renderMarkdown } from '../components/markdown-renderer.js';
import { createEditorOrFallback, LANGS, LANG_BY_ID } from '../components/monaco-loader.js';
import { authStore } from '../store/state.js';
import { navigate } from '../router.js';
import { toast } from '../components/toast.js';
import { formatTime, formatMemory } from '../utils/format.js';
import { makeDraftStore } from '../utils/draft.js';

const DRAFT_PREFIX = 'draft:problem_';
const DEBOUNCE_MS  = 500;

const DEFAULT_LANG = 'cpp';

// 全局草稿存储（注入 localStorage；测试时可替换为 mock）
const draft = makeDraftStore(typeof window !== 'undefined' ? window.localStorage : null);

export default async function problemDetailView(params /*, query */) {
    // 进入新一次 detail 视图：先把上一份 pending 的草稿刷掉（路由切换场景）
    if (_scheduler) {
        try { _scheduler.flush(); } catch {}
        _scheduler = null;
    }

    const problemId = parseInt(params.id, 10);
    if (!Number.isFinite(problemId) || problemId <= 0) {
        return renderNotFound();
    }

    const root = createEl('div', { class: 'view container problem-detail-view' });

    // 顶部：返回 + 标题
    const topBar = createEl('div', { class: 'pd-topbar' });
    topBar.appendChild(createEl('a', {
        class: 'btn btn--ghost btn--sm',
        href: '/problems',
    }, '← 题库'));
    const topTitle = createEl('span', { class: 'pd-topbar__title muted' }, `题目 #${problemId}`);
    topBar.appendChild(topTitle);
    root.appendChild(topBar);

    // 主区域（左 + 右）
    const main = createEl('div', { class: 'pd-main' });
    root.appendChild(main);

    // --- 左栏：题面 ---
    const leftCol = createEl('section', { class: 'pd-left card' });
    main.appendChild(leftCol);

    // --- 右栏：编辑器 ---
    const rightCol = createEl('section', { class: 'pd-right card' });
    main.appendChild(rightCol);

    // 加载题面
    let problem = null;
    try {
        problem = await apiGet(problemId);
    } catch (err) {
        if (err instanceof ApiError && err.code === 1004) {
            main.replaceChildren(empty({
                icon: '404',
                title: '题目不存在',
                hint: '该题目可能被删除或尚未发布',
                action: { label: '返回题库', href: '/problems' },
            }));
            return root;
        }
        main.replaceChildren(errorBanner('加载题目失败：' + (err && err.message || err), {
            onRetry: () => navigate('/problems/' + problemId, { replace: true }),
        }));
        return root;
    }

    // ---- 渲染左栏 ----
    renderLeft(leftCol, problem);

    // ---- 渲染右栏 ----
    const editorState = {
        lang: DEFAULT_LANG,
        editor: null,            // monaco editor 或 textarea
        monaco: null,
        problemId,
        problem,
    };
    renderRight(rightCol, editorState);

    // 离开页面（关闭 tab / 刷新 / 跳路由）时把待写的草稿立即 flush，
    // 避免 debounce 窗口里的最新改动丢失。
    const onBeforeUnload = () => {
        if (_scheduler) _scheduler.flush();
    };
    window.addEventListener('beforeunload', onBeforeUnload);
    // 视图卸载时清掉监听（router 替换 view-root 节点时）
    root._cleanup = () => {
        window.removeEventListener('beforeunload', onBeforeUnload);
        if (_scheduler) _scheduler.flush();
        _scheduler = null;
        if (editorState.editor && editorState.editor.dispose) {
            try { editorState.editor.dispose(); } catch {}
        }
    };

    return root;
}

// =============================================================================
//  左栏：题面 + 样例 + 限制
// =============================================================================
function renderLeft(container, problem) {
    container.replaceChildren();

    // 标题
    const title = createEl('h1', { class: 'pd-title' }, problem.title || '无标题');
    container.appendChild(title);

    // 难度 + 标签
    const meta = createEl('div', { class: 'pd-meta' }, [
        difficultyBadge(problem.difficulty),
    ]);
    if (Array.isArray(problem.tags)) {
        for (const t of problem.tags) {
            const chip = createEl('span', { class: 'tag' }, t.name);
            chip.title = t.slug;
            meta.appendChild(chip);
        }
    }
    container.appendChild(meta);

    // 题面（Markdown）
    const mdBox = createEl('div', { class: 'pd-md' });
    container.appendChild(createEl('h3', { class: 'pd-h3' }, '题目描述'));
    const mdContent = createEl('div', { class: 'pd-md__content' });
    mdContent.textContent = '渲染中…';
    mdBox.appendChild(mdContent);
    container.appendChild(mdBox);

    renderMarkdown(problem.content_md || '').then(html => {
        mdContent.innerHTML = sanitizeRenderedHtml(html);
    }).catch(err => {
        mdContent.textContent = '题面加载失败：' + (err && err.message || err);
    });

    // 样例
    if (Array.isArray(problem.sample_testcases) && problem.sample_testcases.length) {
        const sBox = createEl('div', { class: 'pd-samples' });
        sBox.appendChild(createEl('h3', { class: 'pd-h3' }, '样例'));
        for (const c of problem.sample_testcases) {
            sBox.appendChild(renderSample(c));
        }
        container.appendChild(sBox);
    }

    // 时空限制
    const limits = createEl('div', { class: 'pd-limits muted' }, [
        createEl('span', null, [
            '⏱ ',
            formatTime(problem.time_limit_ms || 2000),
        ]),
        createEl('span', null, [
            ' 💾 ',
            formatMemory((problem.memory_limit_mb || 256) * 1024),
        ]),
        createEl('span', null, [
            ' 📤 输出限制 ',
            (problem.output_limit_mb || 64) + ' MB',
        ]),
    ]);
    container.appendChild(limits);
}

function renderSample(c) {
    const wrap = createEl('div', { class: 'pd-sample' });
    wrap.appendChild(createEl('div', { class: 'pd-sample__title' }, `样例 ${c.case_index}`));
    wrap.appendChild(createEl('div', { class: 'pd-sample__col' }, [
        createEl('div', { class: 'pd-sample__label muted' }, '输入'),
        createEl('pre', { class: 'pd-sample__pre' }, c.input || ''),
    ]));
    wrap.appendChild(createEl('div', { class: 'pd-sample__col' }, [
        createEl('div', { class: 'pd-sample__label muted' }, '输出'),
        createEl('pre', { class: 'pd-sample__pre' }, c.expected_output || ''),
    ]));
    return wrap;
}

// 把 markdown-it 输出的 HTML 做一次白名单兜底（markdown-it 已禁 html，
// 但保险起见再过滤一遍 on*/javascript: 链接）
function sanitizeRenderedHtml(html) {
    return String(html)
        .replace(/<script[\s\S]*?<\/script>/gi, '')
        .replace(/\son\w+="[^"]*"/gi, '')
        .replace(/\son\w+='[^']*'/gi, '')
        .replace(/javascript:/gi, '');
}

// =============================================================================
//  右栏：语言 + Monaco + 提交
// =============================================================================
function renderRight(container, st) {
    container.replaceChildren();

    // 头部：语言选择
    const head = createEl('div', { class: 'pd-right__head' }, [
        createEl('span', { class: 'pd-right__label muted' }, '语言'),
    ]);
    const sel = createEl('select', { class: 'form-select pd-right__lang' });
    for (const L of LANGS) {
        const opt = createEl('option', { value: L.id }, L.label);
        if (L.id === st.lang) opt.selected = true;
        sel.appendChild(opt);
    }
    sel.addEventListener('change', () => {
        // 切语言：保存当前 → 加载新
        const cur = currentCode(st);
        draft.save(st.problemId, st.lang, cur);
        st.lang = sel.value;
        const next = draft.load(st.problemId, st.lang) || (LANG_BY_ID[st.lang] || {}).defaultCode || '';
        mountEditor(editorHost, st, next);
        updateDraftBanner(st, banner);
    });
    head.appendChild(sel);
    container.appendChild(head);

    // 编辑器容器
    const editorHost = createEl('div', { class: 'pd-editor' });
    container.appendChild(editorHost);

    // 初始代码：draft > 默认模板
    const initial = draft.load(st.problemId, st.lang) || (LANG_BY_ID[st.lang] || {}).defaultCode || '';
    mountEditor(editorHost, st, initial);

    // 草稿恢复提示条
    const banner = createEl('div', { class: 'pd-draft-banner' });
    container.appendChild(banner);
    updateDraftBanner(st, banner);

    // 底部按钮
    const actions = createEl('div', { class: 'pd-actions' });
    const resetBtn = createEl('button', {
        class: 'btn btn--secondary',
        type: 'button',
        onClick: () => {
            if (!confirm('确定要重置当前语言的代码为默认模板？\n（不会影响其他语言）')) return;
            const L = LANG_BY_ID[st.lang] || {};
            const def = L.defaultCode || '';
            draft.clear(st.problemId, st.lang);
            setEditorValue(st, def);
            scheduler.cancel();
            updateDraftBanner(st, banner);
        },
    }, '重置');
    const submitBtn = createEl('button', {
        class: 'btn btn--primary',
        type: 'button',
        onClick: () => onSubmit(st, submitBtn),
    }, '提 交');
    actions.appendChild(resetBtn);
    actions.appendChild(submitBtn);
    container.appendChild(actions);

    // 状态条
    const stBar = createEl('div', { class: 'pd-status muted' });
    container.appendChild(stBar);
    st.statusEl = stBar;
}

function currentCode(st) {
    if (!st.editor) return '';
    if (st.editor.kind === 'monaco') return st.editor.editor.getValue();
    if (st.editor.kind === 'textarea') return st.editor.el.value;
    return '';
}

function setEditorValue(st, value) {
    if (!st.editor) return;
    if (st.editor.kind === 'monaco') {
        st.editor.editor.setValue(value || '');
    } else if (st.editor.kind === 'textarea') {
        st.editor.el.value = value || '';
    }
}

let _scheduler = null;
function ensureScheduler(st) {
    if (_scheduler) return _scheduler;
    _scheduler = draft.makeScheduler(
        () => currentCode(st),
        (pid, lang, code) => draft.save(pid, lang, code),
        DEBOUNCE_MS,
    );
    return _scheduler;
}

function scheduleSaveDraft(st) {
    ensureScheduler(st).schedule(st.problemId, st.lang);
}

async function mountEditor(host, st, initialValue) {
    // 拆掉旧的
    host.replaceChildren();
    if (st.editor && st.editor.dispose) {
        try { st.editor.dispose(); } catch {}
        st.editor = null;
    }
    const lang = (LANG_BY_ID[st.lang] || {}).monacoLang || 'plaintext';
    const r = await createEditorOrFallback(host, {
        value: initialValue,
        language: lang,
        onChange: () => scheduleSaveDraft(st),
    });
    st.editor = r;
    if (r.kind === 'textarea') {
        // 降级提示
        host.parentElement.classList.add('pd-editor--fallback');
        if (st.statusEl) {
            st.statusEl.textContent = '⚠ 高级编辑器加载失败，已切换到简单模式';
        }
    }
}

// =============================================================================
//  草稿恢复提示 —— 当用户进入时发现 localStorage 有草稿，提示一下
// =============================================================================
function updateDraftBanner(st, host) {
    if (!host) return;
    const code = draft.load(st.problemId, st.lang);
    if (!code) {
        host.replaceChildren();
        host.classList.remove('is-active');
        return;
    }
    const bytes = new Blob([code]).size;
    const langLabel = (LANGS.find(L => L.id === st.lang) || {}).label || st.lang;
    host.classList.add('is-active');
    host.replaceChildren(
        createEl('span', { class: 'pd-draft-banner__icon' }, '💾'),
        createEl('span', { class: 'pd-draft-banner__text' },
            `已从草稿恢复 [${langLabel}] (${formatBytes(bytes)})`),
        createEl('button', {
            class: 'btn btn--sm btn--ghost pd-draft-banner__clear',
            type: 'button',
            title: '清空当前语言草稿（其他语言不受影响）',
            onClick: () => {
                draft.clear(st.problemId, st.lang);
                const L = LANG_BY_ID[st.lang] || {};
                setEditorValue(st, L.defaultCode || '');
                updateDraftBanner(st, host);
                toast('已清空草稿', 'info');
            },
        }, '清空'),
    );
}

function formatBytes(n) {
    if (n < 1024) return n + ' B';
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1024 / 1024).toFixed(2) + ' MB';
}

// =============================================================================
//  提交
// =============================================================================
async function onSubmit(st, btn) {
    if (!authStore.isLoggedIn) {
        toast('请先登录', 'warn');
        navigate('/login?redirect=' + encodeURIComponent('/problems/' + st.problemId));
        return;
    }

    const code = currentCode(st);
    if (!code.trim()) {
        toast('代码不能为空', 'warn');
        return;
    }

    const L = LANG_BY_ID[st.lang] || {};
    if (!confirm(`确认提交 ${L.label || st.lang} 语言的代码？`)) return;

    btn.disabled = true;
    const oldText = btn.textContent;
    btn.textContent = '提交中…';
    try {
        const data = await apiPost('/submissions', {
            problem_id: st.problemId,
            language:   st.lang,
            code,
        });
        // 清掉 draft
        draft.clear(st.problemId, st.lang);
        if (_scheduler) _scheduler.cancel();
        toast('已提交，判题中…', 'success');
        navigate('/submissions/' + (data && data.submission_id));
    } catch (err) {
        const msg = mapErrMsg(err);
        toast(msg, 'error');
        btn.disabled = false;
        btn.textContent = oldText;
    }
}

function mapErrMsg(err) {
    if (err instanceof ApiError) {
        if (err.code === 1002) return '请先登录';
        if (err.code === 1004) return '题目不存在';
        if (err.code === 1006) return err.message || '代码超长（> 64KB）';
        if (err.code === 1001) return err.message || '请求参数错误';
        if (err.code === 1008) return '服务暂时不可用，请稍后再试';
        return err.message || `提交失败 (code=${err.code})`;
    }
    if (err instanceof HttpError) {
        if (err.status === 0)  return '网络错误，请检查连接';
        if (err.status >= 500) return '服务异常，请稍后再试';
        return `请求失败 (HTTP ${err.status})`;
    }
    return '提交失败：' + (err && err.message || err);
}

// =============================================================================
//  404 占位
// =============================================================================
function renderNotFound() {
    const root = createEl('div', { class: 'view container' });
    root.appendChild(empty({
        icon: '404',
        title: '题目不存在',
        hint: '请检查 URL 是否正确',
        action: { label: '返回题库', href: '/problems' },
    }));
    return root;
}
