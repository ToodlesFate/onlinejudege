// =============================================================================
//  router.js — History API 路由器
//  依据 SPEC §3.3.2 / §3.3.5 B：
//    - 使用 pushState / popstate
//    - pattern 形如 /problems/:id，:id 视为路径参数
//    - 点击 <a href="/..."> 或带 [data-link] 的元素自动拦截走 pushState
//    - 未匹配路径走 notFound 视图
//    - 视图签名：async (params, query) => HTMLElement | { mount, unmount? }
//
//  使用：
//    import { createRouter } from './router.js';
//    const router = createRouter({ mount: '#view-root' });
//    router.add({ path: '/', view: homeView });
//    router.start();
// =============================================================================

/**
 * @typedef {Object} Route
 * @property {string} path           pattern，:name 为路径参数
 * @property {(params: Record<string,string>, query: URLSearchParams) =>
 *           (Node|Promise<Node>)} view
 * @property {string} [name]         可选，路由名
 */

/**
 * @typedef {Object} RouterOptions
 * @property {string|HTMLElement} mount     视图挂载点
 * @property {Route['view']}     [notFound] 未匹配时使用的视图
 * @property {(route: {path:string, params:Record<string,string>, query:URLSearchParams}) => void} [onChange]
 */

/**
 * @param {RouterOptions} opts
 */
export function createRouter(opts) {
    const mountEl = typeof opts.mount === 'string'
        ? document.querySelector(opts.mount)
        : opts.mount;

    if (!mountEl) {
        throw new Error(`[router] mount target not found: ${opts.mount}`);
    }

    /** @type {Route[]} */
    const routes = [];
    let started = false;

    // ---------------------------------------------------------------------
    //  路径匹配：把 '/problems/:id' 编译成正则 + 参数名列表
    // ---------------------------------------------------------------------
    function compile(pattern) {
        const paramNames = [];
        const regex = new RegExp(
            '^' +
            pattern
                .replace(/\/+$/, '')                 // 去尾斜杠
                .replace(/\/:([A-Za-z_][\w]*)/g, (_, name) => {
                    paramNames.push(name);
                    return '/([^/]+)';
                })
                .replace(/\//g, '\\/') +
            '\\/?$'
        );
        return { regex, paramNames };
    }

    /** @returns {{ route: Route, params: Record<string,string> } | null} */
    function match(pathname) {
        for (const r of routes) {
            const { regex, paramNames } = r._compiled;
            const m = pathname.match(regex);
            if (!m) continue;
            const params = {};
            paramNames.forEach((n, i) => { params[n] = decodeURIComponent(m[i + 1]); });
            return { route: r, params };
        }
        return null;
    }

    // ---------------------------------------------------------------------
    //  渲染入口
    // ---------------------------------------------------------------------
    async function render(pathname, { replace = false } = {}) {
        const url = new URL(pathname, location.origin);
        const hit = match(url.pathname);

        const route = hit ? hit.route : null;
        const params = hit ? hit.params : {};
        const query = url.searchParams;

        if (replace) {
            history.replaceState({ pathname: url.pathname }, '', url);
        } else {
            history.pushState({ pathname: url.pathname }, '', url);
        }

        // 触发回调（高亮 nav、scroll 复位等）
        opts.onChange?.({ path: url.pathname, params, query, route });

        const view = route ? route.view : (opts.notFound || defaultNotFound);
        try {
            const result = await view(params, query);
            mountEl.replaceChildren(result);
        } catch (err) {
            console.error('[router] view render failed', err);
            mountEl.replaceChildren(
                Object.assign(document.createElement('div'), {
                    className: 'banner banner--error',
                    textContent: '页面渲染失败：' + (err && err.message || err),
                })
            );
        }
    }

    function defaultNotFound() {
        const el = document.createElement('div');
        el.className = 'empty';
        el.innerHTML = `
            <div class="empty__icon">404</div>
            <div class="empty__title">页面不存在</div>
            <a class="btn btn--primary" href="/" data-link>返回首页</a>
        `;
        return el;
    }

    // ---------------------------------------------------------------------
    //  链接拦截：document 级 click 委托
    // ---------------------------------------------------------------------
    function onDocClick(e) {
        if (e.defaultPrevented) return;
        if (e.button !== 0)        return;
        if (e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;

        const a = e.target instanceof Element
            ? e.target.closest('a[href], [data-link]')
            : null;
        if (!a) return;

        // 显式标注 data-link 一定走 router；<a> 走 router 的条件：同源 + 非 _blank
        const isAnchor = a.tagName === 'A';
        if (isAnchor) {
            if (a.target && a.target !== '_self') return;
            const href = a.getAttribute('href');
            if (!href || href.startsWith('#'))    return;
            if (/^https?:\/\//i.test(href))        return;        // 外链
            if (a.hasAttribute('download'))        return;
        } else if (!a.hasAttribute('data-link')) {
            return;
        }

        const href = a.getAttribute('href') || a.getAttribute('data-link');
        if (!href) return;

        e.preventDefault();
        navigate(href);
    }

    function onPopState() {
        render(location.pathname + location.search, { replace: true });
    }

    // ---------------------------------------------------------------------
    //  公共 API
    // ---------------------------------------------------------------------
    function add(route) {
        if (!route || !route.path || typeof route.view !== 'function') {
            throw new Error('[router] add() requires { path, view }');
        }
        const r = { ...route };
        r._compiled = compile(r.path);
        routes.push(r);
        return r;
    }

    function navigate(path, { replace = false } = {}) {
        if (replace) {
            history.replaceState({}, '', path);
        } else {
            render(path);
        }
    }

    function start() {
        if (started) return;
        started = true;
        document.addEventListener('click', onDocClick);
        window.addEventListener('popstate', onPopState);
        // 首次进入：根据当前 location 渲染
        render(location.pathname + location.search, { replace: true });
    }

    return { add, navigate, start, match, routes };
}
