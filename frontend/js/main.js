// =============================================================================
//  main.js — 前端入口
//  依据 SPEC §3.3.1 / §3.3.2：
//    1) 装配应用壳：app-header / app-main / app-footer
//    2) 注册所有路由（登录/注册为真实视图，其他仍用 stub 占位）
//    3) 启动 History API 路由器
//    4) 初始化 authStore：恢复 localStorage 中的 access_token
//    5) 注入 401 处理钩子 —— refresh 失败时清状态 + 跳登录
// =============================================================================

import { createRouter, navigate } from './router.js';
import { createEl, qs } from './utils/dom.js';

import { authStore } from './store/state.js';
import { setUnauthorizedHandler } from './api/client.js';
import { me as apiMe } from './api/auth.js';

import { renderHeader } from './components/header.js';

import homeView     from './views/home.js';
import loginView    from './views/login.js';
import registerView from './views/register.js';
import problemListView   from './views/problem-list.js';
import problemDetailView from './views/problem-detail.js';
import submissionListView   from './views/submission-list.js';
import submissionDetailView from './views/submission-detail.js';
import notFoundView from './views/not-found.js';
import { stubView } from './views/_stub.js';

// -----------------------------------------------------------------------------
//  路由表 (SPEC §3.3.2)
//  - path:     URL pattern,  :name 为路径参数
//  - view:     async (params, query) => HTMLElement
//  - title:    浏览器标题
// -----------------------------------------------------------------------------
const ROUTES = [
    { path: '/',                          view: homeView,                          title: 'OnlineJudge' },
    { path: '/login',                     view: loginView,                         title: '登录 · OnlineJudge' },
    { path: '/register',                  view: registerView,                      title: '注册 · OnlineJudge' },
    { path: '/problems',                  view: problemListView,                   title: '题库 · OnlineJudge' },
    { path: '/problems/:id',              view: problemDetailView,                 title: '题目详情 · OnlineJudge' },
    { path: '/submissions',               view: submissionListView,               title: '我的提交 · OnlineJudge' },
    { path: '/submissions/:id',           view: submissionDetailView,             title: '提交详情 · OnlineJudge' },
    { path: '/admin/problems',            view: stubView('后台 · 题目管理', 5),     title: '后台 · OnlineJudge' },
    { path: '/admin/problems/new',        view: stubView('新建题目',  5),           title: '新建题目 · OnlineJudge' },
    { path: '/admin/problems/:id/edit',   view: stubView('编辑题目',  5),           title: '编辑题目 · OnlineJudge' },
    { path: '/profile',                   view: stubView('个人资料', 6),           title: '个人资料 · OnlineJudge' },
];

// -----------------------------------------------------------------------------
//  装配应用壳
// -----------------------------------------------------------------------------
const appRoot = qs('#app');
if (!appRoot) throw new Error('mount point #app not found');

const headerEl  = renderHeader();
const mainEl    = createEl('main',  { class: 'app-main',    id: 'view-root' });
const footerEl  = renderFooter();
appRoot.replaceChildren(headerEl, mainEl, footerEl);

function renderFooter() {
    return createEl('footer', { class: 'app-footer' },
        createEl('div', { class: 'app-footer__inner container' }, [
            createEl('div', null, [
                '© ',
                String(new Date().getFullYear()),
                ' OnlineJudge · 仿 LeetCode 风格的在线评测系统',
            ]),
            createEl('div', { class: 'app-footer__meta' }, 'Phase 3 · 题目浏览'),
        ])
    );
}

// -----------------------------------------------------------------------------
//  401 钩子：refresh 失败 → 清状态 + 跳登录（保留 redirect）
// -----------------------------------------------------------------------------
setUnauthorizedHandler(() => {
    // 仅在当前不在登录/注册页时跳转，避免覆盖用户输入
    const p = location.pathname;
    if (p !== '/login' && p !== '/register') {
        const target = '/login?redirect=' + encodeURIComponent(p + location.search);
        navigate(target, { replace: true });
    }
});

// -----------------------------------------------------------------------------
//  路由：注册 + 启动
// -----------------------------------------------------------------------------
const router = createRouter({
    mount: '#view-root',
    notFound: notFoundView,
    onChange: ({ path, route }) => {
        // 高亮当前 nav —— 对 "/problems" 这类顶层 nav，detail 子页也应高亮
        for (const a of Array.from(document.querySelectorAll('a[data-nav]'))) {
            const navPath = a.dataset.nav;
            const active = navPath === route.path
                || (navPath === '/problems'   && path.startsWith('/problems'))
                || (navPath === '/submissions' && path.startsWith('/submissions'))
                || (navPath === '/admin/problems' && path.startsWith('/admin/problems'));
            a.classList.toggle('is-active', active);
        }
        // 浏览器标题
        document.title = (route && route.title) || 'OnlineJudge';
        // 回到顶部
        window.scrollTo({ top: 0, behavior: 'instant' in window ? 'instant' : 'auto' });
    },
});

for (const r of ROUTES) {
    router.add({ path: r.path, view: r.view });
    const registered = router.routes[router.routes.length - 1];
    registered.title = r.title;
}

// -----------------------------------------------------------------------------
//  启动：先恢复 localStorage 的 session，再启动路由；之后异步 me() 校验
// -----------------------------------------------------------------------------
authStore.init();

router.start();

// 启动时若有 token，异步拉一次 me() 确认 user 是最新的（admin 角色可能已被调整）
if (authStore.isLoggedIn) {
    apiMe().catch((err) => {
        // 401 已被 client.js 内部处理过（清 token + 跳登录），这里只兜底
        console.warn('[boot] me() failed:', err && err.message);
    });
}

// -----------------------------------------------------------------------------
//  调试入口：浏览器控制台输入 __oj 可看到 router / store 句柄
// -----------------------------------------------------------------------------
if (typeof window !== 'undefined') {
    window.__oj = { router, authStore };
}
