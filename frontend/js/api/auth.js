// =============================================================================
//  api/auth.js — 认证相关 API (SPEC §5.2.1)
//
//  5 个端点（与 SPEC §5.2.1 一一对应）：
//    POST /api/auth/register   body={username,email,password}            → {user_id,access_token,is_admin}
//    POST /api/auth/login      body={username,password}                  → {user_id,access_token,is_admin}
//    POST /api/auth/refresh    Cookie                                   → {access_token}
//    POST /api/auth/logout     Cookie                                   → null
//    GET  /api/auth/me         Bearer                                   → {user_id,username,email,is_admin}
//
//  成功后由本模块统一把 access_token + user 写入 authStore；
//  失败抛 ApiError，由调用方决定如何展示（Toast / 表单内联错误）。
// =============================================================================

import { apiGet, apiPost } from './client.js';
import { authStore } from '../store/state.js';

// ---------------------------------------------------------------------------
//  私有：从 login/register 响应中构建 user 对象
//  后端在登录/注册的 response data 中只给 {user_id, is_admin, access_token}
//  user_id / is_admin 直接用，username / email 留空，由随后 me() 补全
// ---------------------------------------------------------------------------
function buildUserFromAuthResp(data) {
    if (!data) return null;
    return {
        user_id:  data.user_id,
        username: '',   // 等 me() 回填
        email:    '',
        is_admin: !!data.is_admin,
    };
}

/**
 * 注册。
 * @param {{username: string, email: string, password: string}} payload
 * @returns {Promise<object>}  user
 */
export async function register(payload) {
    const data = await apiPost('/auth/register', payload);
    const user = buildUserFromAuthResp(data);
    authStore.setSession({ user, accessToken: data.access_token });
    // 后台异步拉一次 me 补全 username/email
    me().catch(() => {});
    return user;
}

/**
 * 登录。
 * @param {{username: string, password: string}} payload
 * @returns {Promise<object>}  user
 */
export async function login(payload) {
    const data = await apiPost('/auth/login', payload);
    const user = buildUserFromAuthResp(data);
    authStore.setSession({ user, accessToken: data.access_token });
    me().catch(() => {});
    return user;
}

/**
 * 静默刷新 access token。后端会轮换 refresh cookie。
 * 成功 → 新 access_token 已写入 authStore。
 * 失败 → 抛 ApiError（多为 401，refresh cookie 失效）。
 *
 * @returns {Promise<string>} 新的 access token
 */
export async function refresh() {
    const data = await apiPost('/auth/refresh', null, { _skipAuth: true });
    if (!data || !data.access_token) {
        throw new Error('refresh response missing access_token');
    }
    authStore.setSession({ accessToken: data.access_token });
    return data.access_token;
}

/**
 * 退出登录。清本地状态；不强制依赖 /api/auth/logout 成功
 * （后端若未实现该端点，也不影响"用户视角的登出"）。
 *
 * @returns {Promise<void>}
 */
export async function logout() {
    try {
        await apiPost('/auth/logout', null);
    } catch (e) {
        // 401/404 等都吞掉 —— 本地状态清空才是关键
        console.warn('[auth.logout] backend call failed:', e && e.message);
    } finally {
        authStore.clear();
    }
}

/**
 * 取当前用户信息。Bearer 鉴权。
 * @returns {Promise<{user_id:number, username:string, email:string, is_admin:boolean}>}
 */
export async function me() {
    const data = await apiGet('/auth/me');
    if (!data) throw new Error('me: empty response');
    const user = {
        user_id:  data.user_id,
        username: data.username,
        email:    data.email,
        is_admin: !!data.is_admin,
    };
    authStore.setSession({ user });
    return user;
}
