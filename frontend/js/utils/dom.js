// =============================================================================
//  utils/dom.js — 极简 DOM 工具集
//  依据 SPEC §3.3.1：utils/dom.js 提供 createEl / escapeHtml / qs / qsa
//  SPEC §3.3.5 A 中列出的 loading / empty / pagination 在 Phase 1 不实现，
//  留待对应视图阶段补齐。
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
