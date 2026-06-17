// =============================================================================
//  utils/dom.js — 极简 DOM 工具集
//  依据 SPEC §3.3.1：utils/dom.js 提供 createEl / escapeHtml / qs / qsa
//  + loading / empty / pagination / DifficultyBadge / TagChip / StatusBadge
//  （SPEC §3.3.5 A 中列出的全局共享组件）
// =============================================================================

/**
 * HTML 转义，防止 XSS。
 * @param {unknown} v
 * @returns {string}
 */
export function escapeHtml(v) {
    if (v == null) return '';
    return String(v)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

/**
 * 创建 DOM 元素的极简 helper。
 *
 * @example
 *   createEl('button', { class: 'btn btn--primary', onClick: fn }, '提交')
 *   createEl('div',   { class: 'card' }, [
 *       createEl('h3', null, '标题'),
 *       createEl('p',  null, '描述'),
 *   ])
 *
 * @param {string} tag
 * @param {Record<string, any>|null} attrs  属性对象；on* 走 addEventListener，class 走 className
 * @param {Node|string|null|Array} children
 * @returns {HTMLElement}
 */
export function createEl(tag, attrs = null, children = null) {
    const el = document.createElement(tag);

    if (attrs) {
        for (const [k, v] of Object.entries(attrs)) {
            if (v == null || v === false) continue;

            if (k === 'class' || k === 'className') {
                el.className = Array.isArray(v) ? v.filter(Boolean).join(' ') : String(v);
            } else if (k === 'style' && typeof v === 'object') {
                Object.assign(el.style, v);
            } else if (k === 'dataset' && typeof v === 'object') {
                for (const [dk, dv] of Object.entries(v)) {
                    if (dv != null) el.dataset[dk] = String(dv);
                }
            } else if (k === 'html') {
                // 显式声明 innerHTML，调用方需自己保证可信
                el.innerHTML = String(v);
            } else if (k.startsWith('on') && typeof v === 'function') {
                el.addEventListener(k.slice(2).toLowerCase(), v);
            } else if (k === 'checked' || k === 'disabled' || k === 'selected' || k === 'readOnly') {
                if (v) el[k] = true;
            } else {
                el.setAttribute(k, v === true ? '' : String(v));
            }
        }
    }

    appendChildren(el, children);
    return el;
}

function appendChildren(parent, children) {
    if (children == null || children === false) return;
    if (Array.isArray(children)) {
        for (const c of children) appendChildren(parent, c);
        return;
    }
    if (children instanceof Node) {
        parent.appendChild(children);
        return;
    }
    parent.appendChild(document.createTextNode(String(children)));
}

/** querySelector 简写 */
export const qs  = (sel, root = document) => root.querySelector(sel);

/** querySelectorAll 简写（返回真数组） */
export const qsa = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/**
 * 清空一个节点的全部子节点。
 * @param {Node} node
 */
export function clear(node) {
    if (!node) return;
    while (node.firstChild) node.removeChild(node.firstChild);
}

/**
 * 用给定内容替换一个节点的子树。
 * @param {Node} node
 * @param {Node|string|Array} content
 */
export function setContent(node, content) {
    if (!node) return;
    clear(node);
    appendChildren(node, content);
}

// =============================================================================
//  全局共享组件 (SPEC §3.3.5 A) —— loading / empty / pagination / badge / tag
// =============================================================================

/**
 * 加载占位：居中 spinner + 文字
 * @param {string} [text='加载中...']
 */
export function loading(text = '加载中...') {
    return createEl('div', { class: 'loading-block' }, [
        createEl('div', { class: 'spinner spinner--lg' }),
        createEl('div', { class: 'muted' }, text),
    ]);
}

/**
 * 空状态
 * @param {{ icon?: string, title: string, hint?: string, action?: {label:string, href?:string, onClick?:Function} }} opts
 */
export function empty(opts) {
    const root = createEl('div', { class: 'empty' });
    if (opts.icon)  root.appendChild(createEl('div', { class: 'empty__icon' }, opts.icon));
    root.appendChild(createEl('div', { class: 'empty__title' }, opts.title));
    if (opts.hint)  root.appendChild(createEl('div', { class: 'empty__hint' }, opts.hint));
    if (opts.action) {
        const a = opts.action.href
            ? createEl('a', { class: 'btn btn--primary mt-2', href: opts.action.href }, opts.action.label)
            : createEl('button', { class: 'btn btn--primary mt-2', onClick: opts.action.onClick }, opts.action.label);
        root.appendChild(a);
    }
    return root;
}

/**
 * 通用分页器 —— 上一页 / 中间页码 / 下一页 / 末页 / 共 N 条
 * @param {{ page: number, size: number, total: number, onChange: (page: number) => void }} p
 * @returns {HTMLElement}
 */
export function pagination({ page, size, total, onChange }) {
    const totalPages = Math.max(1, Math.ceil(total / size));
    const root = createEl('div', { class: 'pagination' });

    const info = createEl('span', { class: 'pagination__info' },
        `共 ${total} 条 / ${totalPages} 页`);
    root.appendChild(info);

    const mkBtn = (label, target, { disabled = false, active = false } = {}) => {
        const b = createEl('button', {
            type: 'button',
            disabled: disabled || active,
            class: active ? 'is-active' : '',
        }, label);
        if (!disabled && !active) {
            b.addEventListener('click', () => onChange(target));
        }
        return b;
    };

    // 简化策略：总页 ≤ 7 时全显，否则"1 ... a-1 a a+1 ... last"
    const pages = computePageList(page, totalPages);

    root.appendChild(mkBtn('« 首页', 1,               { disabled: page <= 1 }));
    root.appendChild(mkBtn('‹ 上一页', page - 1,     { disabled: page <= 1 }));

    for (const p of pages) {
        if (p === '…') {
            root.appendChild(createEl('span', { class: 'muted', style: { padding: '0 6px' } }, '…'));
        } else {
            root.appendChild(mkBtn(String(p), p, { active: p === page }));
        }
    }

    root.appendChild(mkBtn('下一页 ›', page + 1,     { disabled: page >= totalPages }));
    root.appendChild(mkBtn('末页 »', totalPages,    { disabled: page >= totalPages }));

    return root;
}

function computePageList(current, total) {
    if (total <= 7) return Array.from({ length: total }, (_, i) => i + 1);
    const list = [1];
    const start = Math.max(2, current - 1);
    const end   = Math.min(total - 1, current + 1);
    if (start > 2)      list.push('…');
    for (let i = start; i <= end; ++i) list.push(i);
    if (end < total - 1) list.push('…');
    list.push(total);
    return list;
}

/**
 * 难度徽章 (SPEC §3.3.5 A DifficultyBadge)
 * @param {'easy'|'medium'|'hard'} d
 * @param {string} [label]  可选覆盖文字
 */
export function difficultyBadge(d, label) {
    const cls = d === 'easy' ? 'badge--easy' : d === 'medium' ? 'badge--medium' : 'badge--hard';
    return createEl('span', { class: `badge ${cls}` }, label || difficultyZh(d));
}

const DIFFICULTY_ZH_INLINE = { easy: '易', medium: '中', hard: '难' };
function difficultyZh(d) {
    return DIFFICULTY_ZH_INLINE[d] || d || '';
}

/**
 * 标签 chip (SPEC §3.3.5 A TagChip)
 * @param {{ id: number, name: string, slug: string }} tag
 * @param {{ active?: boolean, onClick?: (tag) => void }} [opts]
 */
export function tagChip(tag, opts = {}) {
    const el = createEl('button', {
        class: 'tag' + (opts.active ? ' tag--active' : ''),
        type: 'button',
        title: tag.slug,
    }, tag.name);
    if (opts.onClick) {
        el.addEventListener('click', (e) => {
            e.preventDefault();
            e.stopPropagation();
            opts.onClick(tag);
        });
    }
    return el;
}

/**
 * 8 态判题结果徽章 (SPEC §3.3.5 J)
 * @param {string} code  AC/WA/TLE/MLE/OLE/RE/CE/SE
 */
export function statusBadge(code) {
    const LABEL = {
        AC: '通过', WA: '答案错误', TLE: '超时', MLE: '超内存',
        OLE: '输出超限', RE: '运行错误', CE: '编译错误', SE: '系统错误',
        queued: '排队中', compiling: '编译中', running: '运行中',
    };
    const label = LABEL[code] || code;
    const cls = 'badge badge--' + code.toUpperCase();
    return createEl('span', { class: cls }, label);
}

/**
 * 简易 toast-like banner —— 顶部红色错误条 (SPEC §3.3.5 B 5xx)
 * @param {string} message
 * @param {{ onRetry?: () => void }} [opts]
 */
export function errorBanner(message, opts = {}) {
    const root = createEl('div', { class: 'banner banner--error', role: 'alert' }, [
        createEl('span', null, message || '服务异常，请稍后再试'),
        opts.onRetry
            ? createEl('button', { class: 'btn btn--sm btn--secondary', onClick: opts.onRetry }, '重试')
            : null,
    ].filter(Boolean));
    return root;
}
