// =============================================================================
//  components/toast.js — 全局 Toast 通知 (SPEC §3.3.5 A)
//
//  用法：
//    import { toast } from './components/toast.js';
//    toast('保存成功', 'success');
//    toast('用户名已被占用', 'error');
//
//  type: 'info' (default) | 'success' | 'warn' | 'error'
//
//  行为：
//    - 自动创建/复用 .toast-container 节点
//    - 3s 后淡出再移除（淡出动画 200ms）
//    - 同一条消息连续触发会创建多个 toast（不去重，避免信息丢失）
// =============================================================================

const TOAST_TTL_MS = 3000;
const FADE_OUT_MS = 200;

let container = null;
function ensureContainer() {
    if (container && document.body.contains(container)) return container;
    container = document.createElement('div');
    container.className = 'toast-container';
    document.body.appendChild(container);
    return container;
}

/**
 * @param {string} message
 * @param {'info'|'success'|'warn'|'error'} [type]
 * @param {number} [duration]
 * @returns {() => void} 手动 dismiss
 */
export function toast(message, type = 'info', duration = TOAST_TTL_MS) {
    const root = ensureContainer();
    const el = document.createElement('div');
    el.className = `toast toast--${type}`;
    el.setAttribute('role', type === 'error' ? 'alert' : 'status');
    el.textContent = message;
    root.appendChild(el);

    let removed = false;
    const dismiss = () => {
        if (removed) return;
        removed = true;
        el.style.transition = `opacity ${FADE_OUT_MS}ms ease`;
        el.style.opacity = '0';
        setTimeout(() => el.remove(), FADE_OUT_MS);
    };
    // hover 时停掉自动消失（可选 —— 当前实现就让它常驻 3s；保持简洁）
    setTimeout(dismiss, duration);
    return dismiss;
}
