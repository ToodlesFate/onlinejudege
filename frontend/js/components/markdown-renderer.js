// =============================================================================
//  components/markdown-renderer.js — markdown-it CDN 加载 + 渲染 (SPEC §3.3.4)
//
//  行为：
//    - 首次调用 load() 动态插入 <script> 加载 markdown-it@14
//    - 同一会话内只加载一次；后续调用直接返回缓存实例
//    - 失败 → 抛 Error('markdown-it failed to load')
//
//  用法：
//    import { renderMarkdown } from './components/markdown-renderer.js';
//    const html = await renderMarkdown(mdText);
//    container.innerHTML = html;
// =============================================================================

const CDN = 'https://cdn.jsdelivr.net/npm/markdown-it@14.1.0/dist/markdown-it.min.js';

let _loading = null;       // Promise
let _md = null;            // markdownit instance

/**
 * 加载 markdown-it —— 并发安全
 * @returns {Promise<object>} markdownit instance
 */
export function loadMarkdownIt() {
    if (_md) return Promise.resolve(_md);
    if (_loading) return _loading;
    _loading = new Promise((resolve, reject) => {
        // 已被其他模块加载过
        if (typeof window !== 'undefined' && typeof window.markdownit === 'function') {
            _md = window.markdownit({ html: false, linkify: true, breaks: false, typographer: true });
            resolve(_md);
            return;
        }
        const s = document.createElement('script');
        s.src = CDN;
        s.async = true;
        s.onload = () => {
            if (typeof window.markdownit !== 'function') {
                reject(new Error('markdown-it loaded but window.markdownit missing'));
                return;
            }
            _md = window.markdownit({
                html:  false,    // 拒绝内嵌 HTML（安全）
                linkify: true,
                breaks: false,
                typographer: true,
            });
            resolve(_md);
        };
        s.onerror = () => reject(new Error('failed to load markdown-it from CDN'));
        document.head.appendChild(s);
    });
    return _loading;
}

/**
 * 渲染 markdown 文本 → HTML 字符串
 * @param {string} md
 * @returns {Promise<string>}
 */
export async function renderMarkdown(md) {
    if (!md) return '';
    const mdi = await loadMarkdownIt();
    return mdi.render(md);
}
