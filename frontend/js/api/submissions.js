// =============================================================================
//  api/submissions.js — 提交域 API (SPEC §5.2.3)
//
//  4 个端点：
//    POST /api/submissions         body={problem_id, language, code} → {submission_id}
//    GET  /api/submissions/{id}    含逐点状态（SPEC §5.3 完整形状）
//    GET  /api/submissions         ?user=me&page=1&problem_id=&language=&status=
//    GET  /api/submissions/public  仅 result=AC 公开列表
//
//  响应 envelope 由 client.js 统一解出；本模块只暴露 data 字段。
// =============================================================================

import { apiGet, apiPost } from './client.js';

// ---------------------------------------------------------------------------
//  提交创建
// ---------------------------------------------------------------------------

/**
 * @param {{ problem_id: number|string, language: string, code: string }} payload
 * @returns {Promise<{ submission_id: number }>}
 */
export function create(payload) {
    return apiPost('/submissions', {
        problem_id: payload.problem_id,
        language:   payload.language,
        code:       payload.code,
    });
}

// ---------------------------------------------------------------------------
//  详情
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} SubmissionCaseResp
 * @property {number}  case_index
 * @property {string}  status            AC/WA/TLE/MLE/OLE/RE
 * @property {number}  time_used_ms
 * @property {number}  memory_used_kb
 * @property {number}  score
 * @property {boolean} is_sample
 * @property {string|null} user_output       隐藏点为 null
 * @property {string|null} expected_output   隐藏点为 null
 * @property {string|null} input             隐藏点为 null
 */

/**
 * @typedef {Object} SubmissionDetailData
 * @property {number} id
 * @property {number} problem_id
 * @property {number} user_id
 * @property {string} username
 * @property {string} language         c/cpp/java/python/go
 * @property {string} code
 * @property {string} status           queued/compiling/running/finished
 * @property {string|null} result      8 态之一；status=finished 时有值
 * @property {number} total_score
 * @property {number} time_used_ms
 * @property {number} memory_used_kb
 * @property {string} compile_output
 * @property {string} judge_message
 * @property {string} created_at
 * @property {string} finished_at
 * @property {SubmissionCaseResp[]} cases
 */

/**
 * @param {number|string} id
 * @returns {Promise<SubmissionDetailData>}
 */
export function get(id) {
    return apiGet('/submissions/' + encodeURIComponent(id));
}

// ---------------------------------------------------------------------------
//  列表
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} SubmissionListItem
 * @property {number} id
 * @property {number} problem_id
 * @property {string} problem_title
 * @property {string} language
 * @property {string} status
 * @property {string|null} result
 * @property {number} total_score
 * @property {number} time_used_ms
 * @property {number} memory_used_kb
 * @property {string} created_at
 * @property {string} finished_at
 */

/**
 * @typedef {Object} SubmissionListResult
 * @property {SubmissionListItem[]} items
 * @property {number} total
 * @property {number} page
 * @property {number} size
 */

/**
 * 个人提交列表（需 Bearer）。
 * @param {{
 *   page?: number,
 *   size?: number,
 *   problem_id?: number,
 *   language?: string,
 *   status?: string,           // queued/compiling/running/finished/AC/WA/...
 * }} [q]
 * @returns {Promise<SubmissionListResult>}
 */
export function list(q = {}) {
    /** @type {Record<string, any>} */
    const query = { page: q.page ?? 1, size: q.size ?? 20 };
    if (q.problem_id) query.problem_id = q.problem_id;
    if (q.language)   query.language   = q.language;
    if (q.status)     query.status     = q.status;
    return apiGet('/submissions', query);
}

/**
 * 公开 AC 提交列表（无需鉴权）。
 * @param {{ page?: number, size?: number }} [q]
 * @returns {Promise<SubmissionListResult>}
 */
export function listPublic(q = {}) {
    return apiGet('/submissions/public', { page: q.page ?? 1, size: q.size ?? 20 });
}
