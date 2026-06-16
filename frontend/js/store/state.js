// =============================================================================
//  store/state.js — 极简响应式状态 (SPEC §3.3.3)
//
//  设计目标：
//    1) 不引入框架；createSignal 风格，最小 API：get / set / subscribe
//    2) 多个 signal 组成"store"；store 提供聚合订阅
//    3) authStore 持久化 access_token + user 到 localStorage
//       —— refresh token 由后端 HttpOnly Cookie 持有，浏览器自动管理
//
//  API：
//    import { createSignal, authStore } from './store/state.js';
//    const s = createSignal(0);
//    s.set(1);
//    const off = s.subscribe(v => console.log(v));
//    off();  // 取消订阅
// =============================================================================

const LS_USER  = 'oj:user';
const LS_TOKEN = 'oj:access_token';

/**
 * @template T
 * @param {T} initial
 * @returns {{
 *   get: () => T,
 *   set: (v: T) => void,
 *   subscribe: (fn: (v: T) => void) => () => void
 * }}
 */
export function createSignal(initial) {
    let value = initial;
    /** @type {Set<(v: any) => void>} */
    const subs = new Set();

    return {
        get: () => value,
        set: (v) => {
            if (Object.is(v, value)) return;
            value = v;
            // 遍历时允许 unsubscribe（用数组拷贝防迭代中变更）
            for (const fn of Array.from(subs)) {
                try { fn(value); } catch (e) { console.error('[signal subscriber]', e); }
            }
        },
        subscribe: (fn) => {
            subs.add(fn);
            return () => subs.delete(fn);
        },
    };
}

// ---------------------------------------------------------------------------
//  authStore —— 当前用户 + access token（refresh token 由 Cookie 托管）
//
//  注意：setSession() 只更新内存与 localStorage，不调用任何 API；
//  真正的 HTTP 调用（login/register/refresh）由 api/auth.js 负责。
// ---------------------------------------------------------------------------
const _user       = createSignal(/** @type {null | object} */ (null));
const _token      = createSignal(/** @type {null | string} */ (null));
const _ready      = createSignal(false);  // init() 是否已跑完（决定首次 me() 是否触发）

export const authStore = {
    /** @returns {null | { user_id: number, username: string, email: string, is_admin: boolean }} */
    get user()       { return _user.get(); },
    get accessToken() { return _token.get(); },
    get isLoggedIn() { return !!(_user.get() && _token.get()); },
    get isAdmin()    { return !!(_user.get() && _user.get().is_admin); },
    get ready()      { return _ready.get(); },

    /**
     * 写入 session（login / register / refresh / me 都用得到）。
     *  - user / accessToken 任一为 null 视为只更新另一边
     */
    setSession({ user = null, accessToken = null } = {}) {
        if (user !== null) {
            _user.set(user);
            try { localStorage.setItem(LS_USER, JSON.stringify(user)); } catch {}
        }
        if (accessToken !== null) {
            _token.set(accessToken);
            try { localStorage.setItem(LS_TOKEN, accessToken); } catch {}
        }
    },

    /** 清空本地 session（logout / refresh 失败回滚） */
    clear() {
        _user.set(null);
        _token.set(null);
        try { localStorage.removeItem(LS_USER);  } catch {}
        try { localStorage.removeItem(LS_TOKEN); } catch {}
    },

    /**
     * 订阅：user 或 token 任一变化都会触发 fn。
     * @param {(s: { user: object|null, accessToken: string|null, isLoggedIn: boolean, isAdmin: boolean }) => void} fn
     * @returns {() => void} 取消订阅
     */
    subscribe(fn) {
        const wrap = () => fn({
            user: _user.get(),
            accessToken: _token.get(),
            isLoggedIn: !!(_user.get() && _token.get()),
            isAdmin: !!(_user.get() && _user.get().is_admin),
        });
        const offU = _user.subscribe(wrap);
        const offT = _token.subscribe(wrap);
        wrap();  // 立即触发一次
        return () => { offU(); offT(); };
    },

    /**
     * 启动时调用：从 localStorage 恢复；access_token 仍可用时再异步刷一次 me
     * 确认 user 元数据是新的（避免 admin 角色变更后长期不感知）。
     */
    init() {
        if (_ready.get()) return;
        try {
            const u = localStorage.getItem(LS_USER);
            const t = localStorage.getItem(LS_TOKEN);
            if (u && t) {
                _user.set(JSON.parse(u));
                _token.set(t);
            }
        } catch (e) {
            console.warn('[authStore.init] localStorage parse failed', e);
        }
        _ready.set(true);
    },
};
