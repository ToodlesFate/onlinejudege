// =============================================================================
//  views/admin-problem-edit.js — 新建/编辑题目 (SPEC §3.3.5 M)
//
//  路由：
//    /admin/problems/new            → 新建（空表单）
//    /admin/problems/:id/edit      → 编辑（先 GET edit-data 回填）
//
//  布局（两栏，左 60% 题面 / 右 40% 元数据）：
//    ┌────────────────────────────────────────┬───────────────────┐
//    │  ← 返回列表   新建题目 / 编辑题目 #N    │  标题             │
//    ├──────────────────┬─────────────────────┤  难度 ○易○中○难   │
//    │ [编辑][预览]     │ Monaco 渲染结果     │  标签 ☑数组...    │
//    │ Monaco Markdown  │ Markdown 渲染结果   │  时/内/出 限制     │
//    │ 编辑器           │                     │  ☑ 立即发布        │
//    │                  │                     │  [保存草稿][发布]  │
//    └──────────────────┴─────────────────────┴───────────────────┘
//    测试点（动态增删）
//    # │ 输入 │ 预期输出 │ 样例 │ 分值 │ 删除
//
//  行为：
//    - 标题必填、长度 ≤ 100
//    - 难度 / 标签必选
//    - 离开页面前若未保存 → beforeunload 提示
//    - 提交中按钮 disabled
//    - Monaco 加载失败 → 降级 textarea（但仍是 Markdown 模式）
// =============================================================================

import { createEl, loading, empty, errorBanner, difficultyBadge } from '../utils/dom.js';
import { tags as apiTags } from '../api/problems.js';
import * as apiAdmin from '../api/admin-problems.js';
import { createEditorOrFallback } from '../components/monaco-loader.js';
import { renderMarkdown } from '../components/markdown-renderer.js';
import { authStore } from '../store/state.js';
import { navigate } from '../router.js';
import { toast } from '../components/toast.js';
import { ApiError, HttpError } from '../api/client.js';

// 表单默认值（与 SPEC §2.2.1 一致）
const DEFAULTS = {
    title:           '',
    content_md:      '',
    difficulty:      'easy',
    time_limit_ms:   2000,
    memory_limit_mb: 256,
    output_limit_mb: 64,
    is_published:    false,
    tag_ids:         [],
    cases: [
        { case_index: 1, input: '', expected_output: '', is_sample: false, score: 0 },
    ],
};

// 默认题面模板：放进新建题目的 textarea，给 admin 一个 markdown 示例可参考
const DEFAULT_CONTENT_MD =
    `# 题目描述\n\n` +
    `请编写一个程序，读入两个整数 $a$ 和 $b$，输出它们的和。\n\n` +
    `## 输入格式\n\n` +
    `一行两个整数，中间用一个空格分隔。\n\n` +
    `## 输出格式\n\n` +
    `一个整数，表示 $a+b$。\n\n` +
    `## 样例\n\n` +
    `### 样例 1\n\n` +
    `**输入**\n\n` +
    '`1 2`\n\n' +
    `**输出**\n\n` +
    '`3`\n\n' +
    `## 数据范围\n\n` +
    `| 项目 | 范围 |\n` +
    `| --- | --- |\n` +
    `| $a$ | $-10^9 \\le a \\le 10^9$ |\n` +
    `| $b$ | $-10^9 \\le b \\le 10^9$ |\n` +
    `| 时限 | 2 秒 |\n` +
    `| 内存 | 256 MB |\n`;

const MD_TAB_EDIT   = 'edit';
const MD_TAB_PREVIEW = 'preview';

export default async function adminProblemEditView(params /*, query */) {
    if (!authStore.isLoggedIn) {
        const redir = location.pathname;
        navigate('/login?redirect=' + encodeURIComponent(redir));
        return createEl('div', { class: 'view container' });
    }
    if (!authStore.isAdmin) {
        return renderForbidden();
    }

    const isEdit = params && params.id;
    const editId = isEdit ? parseInt(params.id, 10) : 0;
    if (isEdit && (!Number.isFinite(editId) || editId <= 0)) {
        return renderNotFound();
    }

    const root = createEl('div', { class: 'view container ape-view' });

    // ---- 顶部：返回 + 标题 ----
    const topBar = createEl('div', { class: 'pd-topbar ape-topbar' });
    topBar.appendChild(createEl('a', {
        class: 'btn btn--ghost btn--sm',
        href: '/admin/problems',
    }, '← 返回列表'));
    topBar.appendChild(createEl('span', {
        class: 'pd-topbar__title muted',
    }, isEdit ? `编辑题目 #${editId}` : '新建题目'));
    root.appendChild(topBar);

    // ---- 主区：左 60% + 右 40% ----
    const main = createEl('div', { class: 'ape-main' });
    root.appendChild(main);

    const leftCol  = createEl('section', { class: 'ape-left  card' });
    const rightCol = createEl('section', { class: 'ape-right card' });
    main.appendChild(leftCol);
    main.appendChild(rightCol);

    // ---- 测试点区（独立大块，跨左右） ----
    const casesSection = createEl('section', { class: 'ape-cases card' });
    root.appendChild(casesSection);

    // ---- 加载 ----
    let initial = { ...DEFAULTS };
    if (isEdit) {
        leftCol.appendChild(loading('正在加载题目数据...'));
        try {
            const data = await apiAdmin.getEditData(editId);
            initial = {
                title:           data.title || '',
                content_md:      data.content_md || '',
                difficulty:      data.difficulty || 'easy',
                time_limit_ms:   data.time_limit_ms ?? 2000,
                memory_limit_mb: data.memory_limit_mb ?? 256,
                output_limit_mb: data.output_limit_mb ?? 64,
                is_published:    !!data.is_published,
                tag_ids:         Array.isArray(data.tags) ? data.tags.map(t => t.id) : [],
                cases:           Array.isArray(data.cases) && data.cases.length
                    ? data.cases.map(c => ({
                          case_index:      c.case_index,
                          input:           c.input || '',
                          expected_output: c.expected_output || '',
                          is_sample:       !!c.is_sample,
                          score:           c.score || 0,
                      }))
                    : [{ case_index: 1, input: '', expected_output: '', is_sample: false, score: 0 }],
            };
        } catch (err) {
            if (err instanceof ApiError && err.code === 1004) {
                leftCol.replaceChildren(empty({
                    icon: '404',
                    title: '题目不存在',
                    action: { label: '返回列表', href: '/admin/problems' },
                }));
                rightCol.remove();
                casesSection.remove();
                return root;
            }
            if (err instanceof ApiError && err.code === 1003) {
                return renderForbidden();
            }
            leftCol.replaceChildren(errorBanner(
                '加载失败：' + mapErr(err),
                { onRetry: () => navigate(location.pathname, { replace: true }) }
            ));
            rightCol.remove();
            casesSection.remove();
            return root;
        }
    } else {
        initial.content_md = DEFAULT_CONTENT_MD;
    }

    // ---- 标签缓存 ----
    let allTags = [];
    try {
        allTags = await apiTags();
    } catch (e) {
        console.warn('[admin-edit] tags load failed:', e && e.message);
        allTags = [];
    }

    // ---- 状态对象（form state） ----
    const state = {
        isEdit,
        editId,
        title:           initial.title,
        content_md:      initial.content_md,
        difficulty:      initial.difficulty,
        time_limit_ms:   initial.time_limit_ms,
        memory_limit_mb: initial.memory_limit_mb,
        output_limit_mb: initial.output_limit_mb,
        is_published:    initial.is_published,
        tag_ids:         new Set(initial.tag_ids),
        cases:           initial.cases,
        is_dirty:        false,
    };

    // ---- 渲染左栏：题面编辑器 + 预览 ----
    let editorRef = null;          // { kind: 'monaco' | 'textarea', ... }
    let mdTab = MD_TAB_EDIT;
    let mdPreviewHost = null;
    let mdPreviewLastHtml = '';
    let mdPreviewRenderId = 0;
    const mdEditorHost = createEl('div', { class: 'ape-md__editor' });
    const mdTabs = createEl('div', { class: 'ape-md__tabs', role: 'tablist' });
    const tabBtnEdit    = createEl('button', { type: 'button', class: 'ape-md__tab is-active', role: 'tab' }, '编辑');
    const tabBtnPreview = createEl('button', { type: 'button', class: 'ape-md__tab',         role: 'tab' }, '预览');
    mdTabs.appendChild(tabBtnEdit);
    mdTabs.appendChild(tabBtnPreview);

    leftCol.replaceChildren(
        createEl('div', { class: 'ape-md__head' }, [
            createEl('h3', { class: 'pd-h3' }, '题面（Markdown）'),
            mdTabs,
        ]),
        mdEditorHost,
    );
    mdPreviewHost = createEl('div', { class: 'ape-md__preview' });
    mdPreviewHost.style.display = 'none';
    leftCol.appendChild(mdPreviewHost);

    // 启动 Monaco
    editorRef = await createEditorOrFallback(mdEditorHost, {
        value:    state.content_md,
        language: 'markdown',
        onChange: (val) => {
            state.content_md = val;
            markDirty();
            if (mdTab === MD_TAB_PREVIEW) schedulePreviewUpdate();
        },
    });
    if (editorRef.kind === 'monaco') {
        // 切到 markdown 模式（createEditorOrFallback 已用 markdown；这里微调）
        try {
            editorRef.editor.updateOptions({
                wordWrap:           'on',
                lineNumbers:        'on',
                minimap:            { enabled: false },
                fontSize:           13,
                tabSize:            2,
                renderLineHighlight: 'gutter',
            });
            // 让 markdown 字体更易读
            const model = editorRef.editor.getModel();
            if (model) {
                editorRef.editor.createDecorationsCollection([]);
            }
        } catch (e) { /* ignore */ }
    } else {
        // 降级：textarea fallback
        const ta = editorRef.el;
        ta.classList.add('ape-md__fallback');
    }

    function switchTab(tab) {
        mdTab = tab;
        tabBtnEdit.classList.toggle('is-active', tab === MD_TAB_EDIT);
        tabBtnPreview.classList.toggle('is-active', tab === MD_TAB_PREVIEW);
        if (tab === MD_TAB_EDIT) {
            mdEditorHost.style.display = '';
            mdPreviewHost.style.display = 'none';
        } else {
            mdEditorHost.style.display = 'none';
            mdPreviewHost.style.display = '';
            schedulePreviewUpdate(true);
        }
    }
    tabBtnEdit.addEventListener('click', () => switchTab(MD_TAB_EDIT));
    tabBtnPreview.addEventListener('click', () => switchTab(MD_TAB_PREVIEW));

    function currentMd() {
        if (!editorRef) return state.content_md || '';
        if (editorRef.kind === 'monaco')  return editorRef.editor.getValue();
        if (editorRef.kind === 'textarea') return editorRef.el.value;
        return '';
    }

    function schedulePreviewUpdate(force) {
        if (!mdPreviewHost) return;
        const text = currentMd();
        if (!force && text === mdPreviewLastHtml) return;
        const id = ++mdPreviewRenderId;
        mdPreviewHost.replaceChildren(createEl('div', { class: 'muted' }, '渲染中…'));
        renderMarkdown(text).then(html => {
            if (id !== mdPreviewRenderId) return;     // 后续渲染盖掉自己
            mdPreviewLastHtml = text;
            mdPreviewHost.innerHTML = sanitizeMdHtml(html);
        }).catch(err => {
            if (id !== mdPreviewRenderId) return;
            mdPreviewHost.replaceChildren(
                createEl('div', { class: 'banner banner--error' },
                         '渲染失败：' + (err && err.message || err))
            );
        });
    }

    // ---- 渲染右栏：元数据 ----
    rightCol.replaceChildren();
    rightCol.appendChild(createEl('h3', { class: 'pd-h3' }, '元数据'));

    // 标题
    const titleInput = createEl('input', {
        class: 'form-input',
        type: 'text',
        maxLength: '100',
        placeholder: '题目标题（必填，≤ 100 字符）',
        value: state.title,
    });
    titleInput.addEventListener('input', () => {
        state.title = titleInput.value;
        markDirty();
    });
    rightCol.appendChild(createEl('div', { class: 'form-group' }, [
        createEl('label', { class: 'form-label' }, [
            '标题 ',
            createEl('span', { class: 'form-label__required' }, '*'),
        ]),
        titleInput,
    ]));

    // 难度
    const diffRow = createEl('div', { class: 'form-group' });
    diffRow.appendChild(createEl('label', { class: 'form-label' }, [
        '难度 ',
        createEl('span', { class: 'form-label__required' }, '*'),
    ]));
    const diffBox = createEl('div', { class: 'ape-radios' });
    for (const d of ['easy', 'medium', 'hard']) {
        const lbl = createEl('label', { class: 'ape-radio' });
        const r = createEl('input', {
            type: 'radio', name: 'difficulty', value: d,
        });
        if (state.difficulty === d) r.checked = true;
        r.addEventListener('change', () => {
            if (r.checked) {
                state.difficulty = d;
                markDirty();
            }
        });
        lbl.appendChild(r);
        lbl.appendChild(createEl('span', null, difficultyBadge(d)));
        diffBox.appendChild(lbl);
    }
    diffRow.appendChild(diffBox);
    rightCol.appendChild(diffRow);

    // 标签
    const tagGroup = createEl('div', { class: 'form-group' });
    tagGroup.appendChild(createEl('label', { class: 'form-label' }, [
        '标签 ',
        createEl('span', { class: 'form-label__required' }, '*'),
    ]));
    const tagBox = createEl('div', { class: 'ape-tagbox' });
    if (allTags.length) {
        for (const t of allTags) {
            const lbl = createEl('label', { class: 'ape-tagbox__item' });
            const cb = createEl('input', { type: 'checkbox', value: String(t.id) });
            if (state.tag_ids.has(t.id)) cb.checked = true;
            cb.addEventListener('change', () => {
                if (cb.checked) state.tag_ids.add(t.id);
                else            state.tag_ids.delete(t.id);
                markDirty();
            });
            lbl.appendChild(cb);
            lbl.appendChild(createEl('span', null, t.name));
            tagBox.appendChild(lbl);
        }
    } else {
        tagBox.appendChild(createEl('span', { class: 'muted' }, '标签加载失败'));
    }
    tagGroup.appendChild(tagBox);
    rightCol.appendChild(tagGroup);

    // 限制
    rightCol.appendChild(createEl('label', { class: 'form-label' }, '资源限制'));
    const limitsRow = createEl('div', { class: 'ape-limits' });
    limitsRow.appendChild(makeNumberField('时限 (ms)', state.time_limit_ms, 1, 10000, (v) => {
        state.time_limit_ms = v; markDirty();
    }));
    limitsRow.appendChild(makeNumberField('内存 (MB)', state.memory_limit_mb, 64, 1024, (v) => {
        state.memory_limit_mb = v; markDirty();
    }));
    limitsRow.appendChild(makeNumberField('输出 (MB)', state.output_limit_mb, 1, 256, (v) => {
        state.output_limit_mb = v; markDirty();
    }));
    rightCol.appendChild(limitsRow);

    // 立即发布 checkbox
    const pubLabel = createEl('label', { class: 'ape-pub' });
    const pubCb = createEl('input', { type: 'checkbox' });
    pubCb.checked = !!state.is_published;
    pubCb.addEventListener('change', () => {
        state.is_published = pubCb.checked;
        markDirty();
    });
    pubLabel.appendChild(pubCb);
    pubLabel.appendChild(createEl('span', null, '保存后立即发布（对用户可见）'));
    rightCol.appendChild(pubLabel);

    // 按钮区
    const btnRow = createEl('div', { class: 'ape-buttons' });
    const saveDraftBtn = createEl('button', {
        class: 'btn btn--secondary',
        type: 'button',
        onClick: () => submit(false),
    }, '保存草稿');
    const publishBtn = createEl('button', {
        class: 'btn btn--primary',
        type: 'button',
        onClick: () => submit(true),
    }, '发布');
    btnRow.appendChild(saveDraftBtn);
    btnRow.appendChild(publishBtn);
    rightCol.appendChild(btnRow);

    // 状态条
    const statusBar = createEl('div', { class: 'pd-status muted' });
    rightCol.appendChild(statusBar);

    // ---- 渲染测试点 ----
    renderCasesSection();

    function renderCasesSection() {
        casesSection.replaceChildren();
        casesSection.appendChild(createEl('h3', { class: 'pd-h3' }, '测试点'));

        const tbl = createEl('table', { class: 'table ape-cases-table' });
        const thead = createEl('thead', null,
            createEl('tr', null, [
                createEl('th', { style: { width: '40px'  } }, '#'),
                createEl('th', null, '输入'),
                createEl('th', null, '预期输出'),
                createEl('th', { style: { width: '70px'  } }, '样例'),
                createEl('th', { style: { width: '90px'  } }, '分值'),
                createEl('th', { style: { width: '60px'  } }, '操作'),
            ])
        );
        tbl.appendChild(thead);
        const tbody = createEl('tbody');
        for (let i = 0; i < state.cases.length; ++i) {
            tbody.appendChild(renderCaseRow(i));
        }
        tbl.appendChild(tbody);
        casesSection.appendChild(tbl);

        const actions = createEl('div', { class: 'ape-cases-actions' });
        actions.appendChild(createEl('button', {
            class: 'btn btn--sm btn--secondary',
            type: 'button',
            onClick: addCase,
        }, '+ 添加测试点'));
        actions.appendChild(createEl('span', { class: 'muted ape-cases-hint' },
            '共 ' + state.cases.length + ' 个测试点'));
        casesSection.appendChild(actions);
    }

    function renderCaseRow(i) {
        const c = state.cases[i];
        const tr = createEl('tr');

        tr.appendChild(createEl('td', { class: 'muted' }, String(i + 1)));

        const inputTd = createEl('td');
        const inputTa = createEl('textarea', {
            class: 'form-textarea ape-case-textarea',
            rows: '2',
            placeholder: 'input',
        });
        inputTa.value = c.input;
        inputTa.addEventListener('input', () => {
            c.input = inputTa.value;
            markDirty();
        });
        inputTd.appendChild(inputTa);
        tr.appendChild(inputTd);

        const outTd = createEl('td');
        const outTa = createEl('textarea', {
            class: 'form-textarea ape-case-textarea',
            rows: '2',
            placeholder: 'expected_output',
        });
        outTa.value = c.expected_output;
        outTa.addEventListener('input', () => {
            c.expected_output = outTa.value;
            markDirty();
        });
        outTd.appendChild(outTa);
        tr.appendChild(outTd);

        const sampleTd = createEl('td', { class: 'text-center' });
        const sampleCb = createEl('input', { type: 'checkbox', class: 'form-checkbox' });
        sampleCb.checked = !!c.is_sample;
        sampleCb.addEventListener('change', () => {
            c.is_sample = sampleCb.checked;
            markDirty();
        });
        sampleTd.appendChild(sampleCb);
        tr.appendChild(sampleTd);

        const scoreTd = createEl('td');
        const scoreInput = createEl('input', {
            class: 'form-input ape-case-score',
            type: 'number', min: '0', max: '100', step: '1',
        });
        scoreInput.value = String(c.score);
        scoreInput.addEventListener('input', () => {
            const v = parseInt(scoreInput.value, 10);
            c.score = Number.isFinite(v) && v >= 0 ? v : 0;
            markDirty();
        });
        scoreTd.appendChild(scoreInput);
        tr.appendChild(scoreTd);

        const actTd = createEl('td', { class: 'text-center' });
        const del = createEl('button', {
            class: 'btn btn--sm btn--danger',
            type: 'button',
            title: '删除该测试点',
            onClick: () => removeCase(i),
        }, '✕');
        actTd.appendChild(del);
        tr.appendChild(actTd);

        return tr;
    }

    function addCase() {
        state.cases.push({
            case_index:      state.cases.length + 1,
            input:           '',
            expected_output: '',
            is_sample:       false,
            score:           0,
        });
        markDirty();
        renderCasesSection();
    }

    function removeCase(i) {
        if (state.cases.length <= 1) {
            toast('至少保留 1 个测试点', 'warn');
            return;
        }
        if (!confirm(`确认删除测试点 #${i + 1} ？`)) return;
        state.cases.splice(i, 1);
        // 重新编号 case_index（保持 1..N 连续；后端不强制，但前端更易读）
        for (let k = 0; k < state.cases.length; ++k) {
            state.cases[k].case_index = k + 1;
        }
        markDirty();
        renderCasesSection();
    }

    // ---- 提交流程 ----
    async function submit(publish) {
        const valid = validate();
        if (!valid.ok) {
            toast(valid.msg, 'error');
            return;
        }
        // 组装 payload
        const payload = {
            title:           state.title.trim(),
            content_md:      currentMd(),
            difficulty:      state.difficulty,
            time_limit_ms:   state.time_limit_ms,
            memory_limit_mb: state.memory_limit_mb,
            output_limit_mb: state.output_limit_mb,
            is_published:    !!publish,
            tag_ids:         Array.from(state.tag_ids),
            cases:           state.cases.map((c, i) => ({
                case_index:      i + 1,
                input:           c.input || '',
                expected_output: c.expected_output || '',
                is_sample:       !!c.is_sample,
                score:           Number(c.score) || 0,
            })),
        };

        const btn = publish ? publishBtn : saveDraftBtn;
        const allBtns = [saveDraftBtn, publishBtn];
        allBtns.forEach(b => { b.disabled = true; });
        const oldText = btn.textContent;
        btn.textContent = '保存中…';
        statusBar.textContent = '正在提交，请稍候…';

        try {
            let res;
            if (isEdit) {
                res = await apiAdmin.update(editId, payload);
            } else {
                res = await apiAdmin.create(payload);
            }
            state.is_dirty = false;
            const newId = (res && res.id) || (isEdit ? editId : null);
            toast(publish ? '已发布' : '已保存为草稿', 'success');
            if (!isEdit && newId) {
                // 跳到编辑页（让后续保存走 update 路径）
                navigate('/admin/problems/' + newId + '/edit', { replace: true });
            } else {
                statusBar.textContent = '保存成功 ✓';
                // 同步 is_published 状态到 checkbox（避免偏差）
                if (typeof res.is_published === 'boolean') {
                    state.is_published = res.is_published;
                    pubCb.checked = res.is_published;
                }
            }
        } catch (err) {
            statusBar.textContent = '';
            toast('保存失败：' + mapErr(err), 'error');
        } finally {
            allBtns.forEach(b => { b.disabled = false; });
            btn.textContent = oldText;
        }
    }

    function validate() {
        if (!state.title.trim())  return { ok: false, msg: '请填写题目标题' };
        if (state.title.length > 100) return { ok: false, msg: '标题长度不可超过 100 字符' };
        if (!currentMd().trim())  return { ok: false, msg: '题面不能为空' };
        if (state.tag_ids.size === 0) return { ok: false, msg: '请至少选择一个标签' };
        if (state.cases.length === 0) return { ok: false, msg: '请至少添加一个测试点' };
        for (let i = 0; i < state.cases.length; ++i) {
            const c = state.cases[i];
            if (!Number.isFinite(c.score) || c.score < 0) {
                return { ok: false, msg: `测试点 #${i + 1} 的分值无效` };
            }
        }
        return { ok: true };
    }

    function markDirty() {
        state.is_dirty = true;
    }

    // ---- beforeunload 提示 ----
    const onBeforeUnload = (e) => {
        if (state.is_dirty) {
            e.preventDefault();
            e.returnValue = '';
        }
    };
    window.addEventListener('beforeunload', onBeforeUnload);

    // ---- 视图卸载 ----
    root._cleanup = () => {
        window.removeEventListener('beforeunload', onBeforeUnload);
        if (editorRef && editorRef.dispose) {
            try { editorRef.dispose(); } catch {}
        }
    };

    return root;
}

// =============================================================================
//  辅助
// =============================================================================

function makeNumberField(label, value, min, max, onChange) {
    const wrap = createEl('div', { class: 'ape-limit' });
    wrap.appendChild(createEl('label', { class: 'form-label ape-limit__label' }, label));
    const input = createEl('input', {
        class: 'form-input',
        type: 'number',
        min: String(min),
        max: String(max),
        step: '1',
        value: String(value),
    });
    input.addEventListener('input', () => {
        let v = parseInt(input.value, 10);
        if (!Number.isFinite(v)) v = min;
        if (v < min) v = min;
        if (v > max) v = max;
        onChange(v);
    });
    wrap.appendChild(input);
    return wrap;
}

function renderForbidden() {
    return createEl('div', { class: 'view container' }, [
        createEl('div', { class: 'card' }, [
            createEl('h2', null, '403 · 无权限'),
            createEl('p',  { class: 'muted' }, '后台管理仅对管理员开放。'),
            createEl('a', { class: 'btn btn--primary mt-3', href: '/' }, '返回首页'),
        ]),
    ]);
}

function renderNotFound() {
    return createEl('div', { class: 'view container' }, [
        empty({
            icon: '404',
            title: '题目不存在',
            action: { label: '返回列表', href: '/admin/problems' },
        }),
    ]);
}

function mapErr(err) {
    if (err instanceof ApiError) {
        if (err.code === 1002) return '请先登录';
        if (err.code === 1003) return '无权限';
        if (err.code === 1004) return '题目不存在';
        if (err.code === 1001) return err.message || '请求参数错误';
        return err.message || `错误 (code=${err.code})`;
    }
    if (err instanceof HttpError) {
        if (err.status === 0)  return '网络错误';
        if (err.status >= 500) return '服务异常';
        return `HTTP ${err.status}`;
    }
    return (err && err.message) || String(err);
}

// markdown-it 已禁 html；这里再过滤 on* / javascript: 链接做兜底
function sanitizeMdHtml(html) {
    return String(html)
        .replace(/<script[\s\S]*?<\/script>/gi, '')
        .replace(/\son\w+="[^"]*"/gi, '')
        .replace(/\son\w+='[^']*'/gi, '')
        .replace(/javascript:/gi, '');
}
