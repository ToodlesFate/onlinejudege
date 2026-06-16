// =============================================================================
//  main.js — 前端入口
//  依据 SPEC §3.3.1 / §3.3.2：
//    1) 装配应用壳：app-header / app-main / app-footer
//    2) 注册所有路由（仅首页与 404 为真实视图，其余用 stub 占位）
//    3) 启动 History API 路由器
// =============================================================================

import { createRouter } from './router.js';
import { createEl, escapeHtml, qs, setContent } from './utils/dom.js';

import homeView     from './views/home.js';
import notFoundView from './views/not-found.js';
import { stubView } from './views/_stub.js';

// -----------------------------------------------------------------------------
//  路由表 (SPEC §3.3.2)
//  - path:     URL pattern,  :name 为路径参数
//  - view:     async (params, query) => HTMLElement
//  - nav:      是否在顶部导航中显示
//  - title:    浏览器标题
//  - phase:    计划落地的 Phase（仅 stub 用）
// -----------------------------------------------------------------------------
const ROUTES = [
    { path: '/',                          view: homeView,                          nav: false, title: 'OnlineJudge' },
    { path: '/login',                     view: stubView('登录',   2),             nav: { label: '登录',   location: 'right' }, title: '登录 · OnlineJudge' },
    { path: '/register',                  view: stubView('注册',   2),             nav: { label: '注册',   location: 'right' }, title: '注册 · OnlineJudge' },
    { path: '/problems',                  view: stubView('题目列表', 3),           nav: { label: '题库',   location: 'left'  }, title: '题库 · OnlineJudge' },
    { path: '/problems/:id',              view: stubView('题目详情', 3),           nav: false, title: '题目详情 · OnlineJudge' },
    { path: '/submissions',               view: stubView('我的提交', 6),           nav: { label: '提交',   location: 'left'  }, title: '我的提交 · OnlineJudge' },
    { path: '/submissions/:id',           view: stubView('提交详情', 6),           nav: false, title: '提交详情 · OnlineJudge' },
    { path: '/admin/problems',            view: stubView('后台 · 题目管理', 5),     nav: { label: '后台',   location: 'left',  admin: true }, title: '后台 · OnlineJudge' },
    { path: '/admin/problems/new',        view: stubView('新建题目',  5),           nav: false, title: '新建题目 · OnlineJudge' },
    { path: '/admin/problems/:id/edit',   view: stubView('编辑题目',  5),           nav: false, title: '编辑题目 · OnlineJudge' },
    { path: '/profile',                   view: stubView('个人资料', 6),           nav: false, title: '个人资料 · OnlineJudge' },
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

function renderHeader() {
    const brand = createEl('a', { class: 'app-header__brand', href: '/' }, [
        createEl('span', { class: 'app-header__brand-mark' }),
        createEl('span', null, 'OnlineJudge'),
    ]);

    const leftNav  = createEl('nav', { class: 'app-header__nav', id: 'nav-left'  });
    const rightNav = createEl('nav', { class: 'app-header__nav', id: 'nav-right' });
    const userSlot = createEl('div', { class: 'app-header__user', id: 'user-slot' }, [
        // Phase 2 落地后换成真实用户菜单；目前显示当前路由 phase
        createEl('span', { class: 'badge' }, 'Phase 1'),
    ]);

    for (const r of ROUTES) {
        if (!r.nav) continue;
        const a = createEl('a', {
            href: r.path,
            'data-nav': r.path,
            'data-nav-loc': r.nav.location,
        }, r.nav.label);
        (r.nav.location === 'right' ? rightNav : leftNav).appendChild(a);
    }

    return createEl('header', { class: 'app-header' },
        createEl('div', { class: 'app-header__inner container' },
            [brand, leftNav, rightNav, userSlot]
        )
    );
}

function renderFooter() {
    return createEl('footer', { class: 'app-footer' },
        createEl('div', { class: 'app-footer__inner container' }, [
            createEl('div', null, [
                '© ',
                String(new Date().getFullYear()),
                ' OnlineJudge · 仿 LeetCode 风格的在线评测系统',
            ]),
            createEl('div', { class: 'app-footer__meta' }, 'Phase 1 · 基础骨架'),
        ])
    );
}

// -----------------------------------------------------------------------------
//  路由：注册 + 启动
// -----------------------------------------------------------------------------
const router = createRouter({
    mount: '#view-root',
    notFound: notFoundView,
    onChange: ({ path, route }) => {
        // 高亮当前 nav
        for (const a of qsa('a[data-nav]')) {
            a.classList.toggle('is-active', a.dataset.nav === route.path);
        }
        // 浏览器标题
        document.title = (route && route.title) || 'OnlineJudge';
        // 回到顶部
        window.scrollTo({ top: 0, behavior: 'instant' in window ? 'instant' : 'auto' });
    },
});

for (const r of ROUTES) {
    router.add({
        path: r.path,
        view: r.view,
    });
    // 把 ROUTES 里的元信息挂到 _compiled 同一层方便 onChange 读取
    const registered = router.routes[router.routes.length - 1];
    registered.title = r.title;
}

router.start();

// -----------------------------------------------------------------------------
//  调试入口：浏览器控制台输入 __oj 可看到 router 句柄
// -----------------------------------------------------------------------------
if (typeof window !== 'undefined') {
    window.__oj = { router };
}
