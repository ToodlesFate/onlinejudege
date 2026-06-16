// =============================================================================
//  api/client.js — fetch 封装 + 401 静默刷新 (SPEC §3.3.1 / §2.1 / §3.3.5 B)
//
//  设计要点：
//    1) 统一 base = '/api'；所有方法返回 data 字段，envelope 由 client 解析
//    2) Access Token 自动注入 `Authorization: Bearer <token>` (Header)
//    3) Refresh Token 走 HttpOnly Cookie —— 浏览器自动随请求发送
//    4) HTTP 401 → 自动 POST /api/auth/refresh 一次 → 用新 token 重试原请求
//       · 多个并发 401 共享同一个 refresh Promise（避免雪崩）
//       · refresh 失败 → 触发 onUnauthorized 钩子（清状态 + 跳登录）
//    5) 业务错误（code != 0）抛 ApiError(code, message, status, data)
//    6) 网络/HTTP 错误抛 HttpError(status, message)
//
//  用法：
//    import { apiGet, apiPost, ApiError } from './api/client.js';
//    const data = await apiGet('/problems', { page: 1 });
// =============================================================================

import { authStore } from '../store/state.js';

const BASE = '/api';

// ---------------------------------------------------------------------------
//  错误类型
// ---------------------------------------------------------------------------
export class ApiError extends Error {
    /**
     * @param {number} code      业务错误码（SPEC §5.1：1001~1008）
     * @param {string} message
     * @param {number} status    HTTP 状态码
     * @param {any}    data
     */
    constructor(code, message, status, data) {
        super(message || `api error ${code}`);
        this.name    = 'ApiError';
        this.code    = code;
        this.status  = status;
        this.data    = data;
    }
}

export class HttpError extends Error {
    constructor(status, message) {
        super(message || `http ${status}`);
        this.name   = 'HttpError';
        this.status = status;
    }
}

// ---------------------------------------------------------------------------
//  401 处理钩子（main.js 注入：清 token + 跳 /login）
// ---------------------------------------------------------------------------
/** @type {() => void} */
let onUnauthorized = () => {};
export function setUnauthorizedHandler(fn) { onUnauthorized = fn || (() => {}); }

// ---------------------------------------------------------------------------
//  共享 refresh Promise —— 多个并发 401 只触发一次 refresh
// ---------------------------------------------------------------------------
let refreshingPromise = null;

/** @returns {Promise<string>} 新的 access token */
async function doRefresh() {
    if (refreshingPromise) return refreshingPromise;
    refreshingPromise = (async () => {
        const res = await fetch(BASE + '/auth/refresh', {
            method: 'POST',
            credentials: 'same-origin',
            headers: { 'Accept': 'application/json' },
        });
        if (!res.ok) {
            // 401 / 500 / 404 等 —— refresh token 不可用
            throw new Error('refresh failed with HTTP ' + res.status);
        }
        const body = await safeJson(res);
        if (!body || body.code !== 0 || !body.data || !body.data.access_token) {
            throw new Error('refresh response invalid: ' + (body && body.message));
        }
        return body.data.access_token;
    })();
    try {
        return await refreshingPromise;
    } finally {
        // 下一个 tick 清掉，让下一次 401 重新触发（成功路径不重置）
        setTimeout(() => { refreshingPromise = null; }, 0);
    }
}

// ---------------------------------------------------------------------------
//  JSON 解析容错
// ---------------------------------------------------------------------------
async function safeJson(res) {
    const text = await res.text();
    if (!text) return null;
    try { return JSON.parse(text); } catch { return null; }
}

// ---------------------------------------------------------------------------
//  底层 fetch（不挂 Authorization，由 apiFetch 注入）
// ---------------------------------------------------------------------------
function buildHeaders(extra) {
    /** @type {Record<string,string>} */
    const h = { 'Accept': 'application/json' };
    if (extra && typeof extra === 'object') {
        for (const [k, v] of Object.entries(extra)) {
            if (v != null) h[k] = String(v);
        }
    }
    return h;
}

function buildBody(body) {
    if (body == null) return undefined;
    if (body instanceof FormData) return body;
    return JSON.stringify(body);
}

// ---------------------------------------------------------------------------
//  核心：apiFetch
// ---------------------------------------------------------------------------
const REFRESH_PATH = '/auth/refresh';

/**
 * @param {string} path
 * @param {{
 *   method?: 'GET'|'POST'|'PUT'|'PATCH'|'DELETE',
 *   body?: any,
 *   query?: Record<string, any>,
 *   headers?: Record<string, string>,
 *   credentials?: 'same-origin'|'include'|'omit',
 *   _isRetry?: boolean,        // 内部用：retry 时不再尝试 refresh
 *   _skipAuth?: boolean,       // 内部用：不注入 Authorization
 * }} [opts]
 * @returns {Promise<any>}       response.data
 */
export async function apiFetch(path, opts = {}) {
    const method = (opts.method || 'GET').toUpperCase();
    const url = BASE + path + (opts.query ? '?' + buildQuery(opts.query) : '');

    /** @type {Record<string,string>} */
    const headers = buildHeaders(opts.headers);
    if (opts.body != null && !(opts.body instanceof FormData) && !headers['Content-Type']) {
        headers['Content-Type'] = 'application/json';
    }
    if (!opts._skipAuth) {
        const tok = authStore.accessToken;
        if (tok) headers['Authorization'] = 'Bearer ' + tok;
    }

    let res;
    try {
        res = await fetch(url, {
            method,
            headers,
            body: buildBody(opts.body),
            credentials: opts.credentials || 'same-origin',
        });
    } catch (e) {
        // 网络错误（断网 / CORS / DNS …）
        throw new HttpError(0, e && e.message || 'network error');
    }

    // 401 自动 refresh 一次
    if (res.status === 401 && !opts._isRetry && !path.endsWith(REFRESH_PATH)) {
        try {
            const newToken = await doRefresh();
            authStore.setSession({ accessToken: newToken });
            // 透传其余 opts，标记 _isRetry 防止再 refresh
            return apiFetch(path, { ...opts, _isRetry: true });
        } catch (e) {
            // refresh 失败 → 清状态 + 通知调用方
            authStore.clear();
            try { onUnauthorized(); } catch (err) { console.error('[onUnauthorized]', err); }
            // 解析一下 body，抛出更具体的 ApiError
            const body = await safeJson(res);
            if (body && typeof body === 'object' && 'code' in body) {
                throw new ApiError(body.code, body.message, 401, body.data);
            }
            throw new ApiError(1002, 'unauthorized', 401, null);
        }
    }

    // 非 2xx 解析 envelope 后抛 ApiError；解析不到抛 HttpError
    if (!res.ok) {
        const body = await safeJson(res);
        if (body && typeof body === 'object' && 'code' in body) {
            throw new ApiError(body.code, body.message, res.status, body.data);
        }
        throw new HttpError(res.status, res.statusText);
    }

    // 2xx：尝试解析 envelope
    const body = await safeJson(res);
    if (body == null) return null;
    if (typeof body === 'object' && 'code' in body) {
        if (body.code !== 0) {
            throw new ApiError(body.code, body.message, res.status, body.data);
        }
        return body.data;
    }
    return body;
}

function buildQuery(q) {
    const sp = new URLSearchParams();
    for (const [k, v] of Object.entries(q)) {
        if (v == null || v === '') continue;
        sp.set(k, String(v));
    }
    return sp.toString();
}

// ---------------------------------------------------------------------------
//  便捷方法
// ---------------------------------------------------------------------------
export const apiGet    = (path, query, opts = {})        => apiFetch(path, { ...opts, method: 'GET',    query });
export const apiPost   = (path, body,   opts = {})        => apiFetch(path, { ...opts, method: 'POST',   body });
export const apiPut    = (path, body,   opts = {})        => apiFetch(path, { ...opts, method: 'PUT',    body });
export const apiPatch  = (path, body,   opts = {})        => apiFetch(path, { ...opts, method: 'PATCH',  body });
export const apiDelete = (path, body,   opts = {})        => apiFetch(path, { ...opts, method: 'DELETE', body });
