// =============================================================================
//  api/admin-problems.js — 后台管理题目域 API (SPEC §5.2.4 / §3.3.5 L,M)
//
//  5 个端点（全部 admin 鉴权）：
//    GET    /api/admin/problems                 ?page=&size=&q=&is_published=
//    GET    /api/admin/problems/:id/edit-data   含完整 testcases + tags
//    POST   /api/admin/problems                 body: 完整题目 + cases[]
//    PUT    /api/admin/problems/:id             body: 完整题目 + cases[]
//    DELETE /api/admin/problems/:id             软删（is_published=0）
//    PATCH  /api/admin/problems/:id/publish     body: {is_published}
//
//  响应 envelope 由 client.js 统一解出；本模块只暴露 data 字段。
// =============================================================================

import { apiGet, apiPost, apiPut, apiPatch, apiDelete } from './client.js';

// ---------------------------------------------------------------------------
//  类型定义
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} AdminProblemListItem
 * @property {number} id
 * @property {string} title
 * @property {'easy'|'medium'|'hard'} difficulty
 * @property {boolean} is_published
 * @property {number} created_by
 * @property {string} created_at
 * @property {{id:number,name:string,slug:string}[]} tags
 * @property {{total:number, accepted:number, pass_rate:number}} stats
 */

/**
 * @typedef {Object} AdminProblemListResult
 * @property {AdminProblemListItem[]} items
 * @property {number} total
 * @property {number} page
 * @property {number} size
 */

/**
 * @typedef {Object} AdminProblemTag
 * @property {number} id
 * @property {string} name
 * @property {string} slug
 */

/**
 * @typedef {Object} AdminProblemCase
 * @property {number} case_index
 * @property {string} input
 * @property {string} expected_output
 * @property {boolean} is_sample
 * @property {number} score
 */

/**
 * @typedef {Object} AdminProblemDetail
 * @property {number} id
 * @property {string} title
 * @property {string} content_md
 * @property {'easy'|'medium'|'hard'} difficulty
 * @property {number} time_limit_ms
 * @property {number} memory_limit_mb
 * @property {number} output_limit_mb
 * @property {boolean} is_published
 * @property {number} created_by
 * @property {string} created_at
 * @property {AdminProblemTag[]} tags
 * @property {AdminProblemCase[]} cases
 */

/**
 * @typedef {Object} AdminProblemWritePayload
 * @property {string}  title
 * @property {string}  content_md
 * @property {'easy'|'medium'|'hard'} difficulty
 * @property {number}  time_limit_ms
 * @property {number}  memory_limit_mb
 * @property {number}  output_limit_mb
 * @property {boolean} is_published
 * @property {number[]} tag_ids
 * @property {AdminProblemCase[]} cases
 */

// ---------------------------------------------------------------------------
//  列表 (GET /api/admin/problems)
// ---------------------------------------------------------------------------

/**
 * @param {{
 *   page?: number,
 *   size?: number,
 *   q?: string,
 *   is_published?: boolean|'1'|'0',
 *   include_unpublished?: '0'|'1',  // 显式覆盖：admin 默认 true
 * }} [q]
 * @returns {Promise<AdminProblemListResult>}
 */
export function list(q = {}) {
    /** @type {Record<string, any>} */
    const query = {
        page: q.page ?? 1,
        size: q.size ?? 20,
    };
    if (q.q)                     query.q                   = q.q;
    if (q.is_published != null)  query.is_published        = String(q.is_published);
    if (q.include_unpublished)   query.include_unpublished = q.include_unpublished;
    return apiGet('/admin/problems', query);
}

// ---------------------------------------------------------------------------
//  编辑数据 (GET /api/admin/problems/:id/edit-data)
// ---------------------------------------------------------------------------

/**
 * @param {number|string} id
 * @returns {Promise<AdminProblemDetail>}
 */
export function getEditData(id) {
    return apiGet('/admin/problems/' + encodeURIComponent(id) + '/edit-data');
}

// ---------------------------------------------------------------------------
//  创建 (POST /api/admin/problems)
// ---------------------------------------------------------------------------

/**
 * @param {AdminProblemWritePayload} payload
 * @returns {Promise<AdminProblemDetail>}
 */
export function create(payload) {
    return apiPost('/admin/problems', serializeWritePayload(payload));
}

// ---------------------------------------------------------------------------
//  更新 (PUT /api/admin/problems/:id)
// ---------------------------------------------------------------------------

/**
 * @param {number|string} id
 * @param {AdminProblemWritePayload} payload
 * @returns {Promise<AdminProblemDetail>}
 */
export function update(id, payload) {
    return apiPut('/admin/problems/' + encodeURIComponent(id),
                  serializeWritePayload(payload));
}

// ---------------------------------------------------------------------------
//  软删 (DELETE /api/admin/problems/:id)
// ---------------------------------------------------------------------------

/**
 * @param {number|string} id
 * @returns {Promise<null>}
 */
export function remove(id) {
    return apiDelete('/admin/problems/' + encodeURIComponent(id));
}

// ---------------------------------------------------------------------------
//  上下架 (PATCH /api/admin/problems/:id/publish)
// ---------------------------------------------------------------------------

/**
 * @param {number|string} id
 * @param {boolean} is_published
 * @returns {Promise<AdminProblemDetail>}
 */
export function setPublished(id, is_published) {
    return apiPatch('/admin/problems/' + encodeURIComponent(id) + '/publish', {
        is_published: !!is_published,
    });
}

// ---------------------------------------------------------------------------
//  序列化：把 edit-form 的友好 payload 转成后端期望的形态
//  边界：tag_ids / cases 缺省时补空数组
// ---------------------------------------------------------------------------
function serializeWritePayload(p) {
    return {
        title:           p.title || '',
        content_md:      p.content_md || '',
        difficulty:      p.difficulty || 'easy',
        time_limit_ms:   p.time_limit_ms ?? 2000,
        memory_limit_mb: p.memory_limit_mb ?? 256,
        output_limit_mb: p.output_limit_mb ?? 64,
        is_published:    !!p.is_published,
        tag_ids:         Array.isArray(p.tag_ids) ? p.tag_ids.filter(n => n > 0) : [],
        cases:           Array.isArray(p.cases) ? p.cases.map((c, i) => ({
            case_index:      c.case_index ?? (i + 1),
            input:           c.input || '',
            expected_output: c.expected_output || '',
            is_sample:       !!c.is_sample,
            score:           Number.isFinite(c.score) ? c.score : 0,
        })) : [],
    };
}
