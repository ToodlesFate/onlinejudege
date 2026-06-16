// =============================================================================
//  components/header.js — 全局顶部导航 (SPEC §3.3.5 A)
//
//  结构（与 main.js 装配的 .app-header 完全兼容）：
//    <header class="app-header">
//      <div class="app-header__inner container">
//        <a class="app-header__brand">OnlineJudge</a>
//        <nav id="nav-left">...</nav>
//        <nav id="nav-right">...</nav>
//        <div id="user-slot">...</div>
//      </div>
//    </header>
//
//  行为：
//    - 左侧 nav：题库、提交、（admin 时）后台
//    - 右侧 nav：未登录显示 [登录] [注册]；登录后替换为用户菜单（username ▾）
//    - 用户菜单：个人资料、退出登录、（admin 时）后台
//    - 订阅 authStore 实时更新
// =============================================================================

import { createEl, qs } from '../utils/dom.js';
import { authStore } from '../store/state.js';
import { logout as authLogout } from '../api/auth.js';
import { toast } from './toast.js';
import { navigate } from '../router.js';

// ---------------------------------------------------------------------------
//  路由元信息 —— 与 main.js ROUTES 对齐
//  Phase 2 落地后 main.js 也会从这里导入
// ---------------------------------------------------------------------------
const NAV_ITEMS = [
    { path: '/problems',   label: '题库', location: 'left'  },
    { path: '/submissions',label: '提交', location: 'left'  },
    { path: '/admin/problems', label: '后台', location: 'left', admin: true },
    { path: '/login',      label: '登录', location: 'right' },
    { path: '/register',   label: '注册', location: 'right' },
];

// ---------------------------------------------------------------------------
//  渲染：单次输出整个 header
// ---------------------------------------------------------------------------
export function renderHeader() {
    const brand = createEl('a', { class: 'app-header__brand', href: '/' }, [
        createEl('span', { class: 'app-header__brand-mark' }),
        createEl('span', null, 'OnlineJudge'),
    ]);

    const leftNav  = createEl('nav', { class: 'app-header__nav', id: 'nav-left'  });
    const rightNav = createEl('nav', { class: 'app-header__nav', id: 'nav-right' });
    const userSlot = createEl('div', { class: 'app-header__user', id: 'user-slot' });

    // 订阅 authStore —— user / isLoggedIn 变化时重渲 nav + userSlot
    authStore.subscribe(({ isLoggedIn, isAdmin }) => {
        // 清空再填
        leftNav.replaceChildren();
        rightNav.replaceChildren();
        userSlot.replaceChildren();

        const isAuthed = isLoggedIn;

        for (const item of NAV_ITEMS) {
            if (item.admin && !isAdmin) continue;       // 隐藏后台
            if (item.path === '/login' || item.path === '/register') {
                if (isAuthed) continue;                 // 登录后隐藏登录/注册
            }
            const a = createEl('a', {
                href: item.path,
                'data-nav': item.path,
                'data-nav-loc': item.location,
            }, item.label);
            (item.location === 'right' ? rightNav : leftNav).appendChild(a);
        }

        if (isAuthed) {
            userSlot.appendChild(renderUserMenu());
        }
        // 未登录时 userSlot 留空 —— 登录/注册入口在 rightNav
    });

    return createEl('header', { class: 'app-header' },
        createEl('div', { class: 'app-header__inner container' },
            [brand, leftNav, rightNav, userSlot]
        )
    );
}

// ---------------------------------------------------------------------------
//  用户菜单 —— 按钮 + 下拉
// ---------------------------------------------------------------------------
function renderUserMenu() {
    const user = authStore.user;
    const triggerLabel = user && user.username ? user.username : '我';

    const trigger = createEl('button', {
        class: 'user-menu__trigger',
        type: 'button',
        'aria-haspopup': 'menu',
        'aria-expanded': 'false',
    }, [
        createEl('span', { class: 'user-menu__avatar' }, initialsOf(user)),
        createEl('span', null, triggerLabel),
        createEl('span', { class: 'user-menu__caret', 'aria-hidden': 'true' }, '▾'),
    ]);

    const menu = createEl('div', { class: 'user-menu__dropdown', role: 'menu' }, [
        menuLink('/profile',  '个人资料'),
        authStore.isAdmin ? menuLink('/admin/problems', '后台管理') : null,
        createEl('hr', { class: 'user-menu__sep' }),
        createEl('button', {
            class: 'user-menu__item user-menu__item--danger',
            type: 'button',
            role: 'menuitem',
            onClick: onLogoutClick,
        }, '退出登录'),
    ].filter(Boolean));

    const wrap = createEl('div', { class: 'user-menu' }, [trigger, menu]);

    // 点击 trigger 切换展开；点击外部 / Esc 关闭
    let open = false;
    function setOpen(v) {
        open = v;
        menu.classList.toggle('is-open', open);
        trigger.setAttribute('aria-expanded', String(open));
    }
    trigger.addEventListener('click', (e) => { e.stopPropagation(); setOpen(!open); });
    document.addEventListener('click', () => setOpen(false));
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') setOpen(false); });

    return wrap;
}

function menuLink(href, label) {
    return createEl('a', { class: 'user-menu__item', href, role: 'menuitem' }, label);
}

function initialsOf(user) {
    if (!user || !user.username) return '?';
    const s = user.username.trim();
    return s.charAt(0).toUpperCase();
}

// ---------------------------------------------------------------------------
//  退出登录
// ---------------------------------------------------------------------------
async function onLogoutClick() {
    if (!window.confirm('确定要退出登录吗？')) return;
    try {
        await authLogout();
    } finally {
        toast('已退出登录', 'success');
        // 强制走 router 让 URL 干净地落到 / ；直接 location.assign 也可
        navigate('/', { replace: true });
        // 触发一次 popstate 让 router 重渲当前页（如 /submissions）
        window.dispatchEvent(new PopStateEvent('popstate'));
    }
}
