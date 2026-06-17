// =============================================================================
//  api/problems.js — 题目域 API (SPEC §5.2.2)
//
//  3 个端点（全部公开，鉴权=否）：
//    GET /api/problems           列表 + 分页 + 过滤 + 排序
//    GET /api/problems/:id       详情（题面 + 样例点 + 标签）
//    GET /api/tags              8 个预置标签
//
//  响应统一 envelope 由 client.js 解析；本模块只暴露 data 字段。
// =============================================================================

import { apiGet } from './client.js';

// ---------------------------------------------------------------------------
//  列表 —— 支持 query 过滤
//  对应 SPEC §5.2.2 GET /api/problems?page=1&size=20&difficulty=&tag=&sort=&q=
//  多 tag 是 AND 关系，后端把 tag 重复出现视为 AND 过滤。
//  本模块允许传 tags: string[]，自动用同名参数多次 set。
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} ProblemListQuery
 * @property {number} [page=1]            页号 (1-based)
 * @property {number} [size=20]           每页条数 (10 / 20 / 50)
 * @property {'easy'|'medium'|'hard'} [difficulty]
 * @property {string[]} [tags]            多选 AND
 * @property {string} [sort]              created_desc / pass_rate_desc / id_desc / id_asc
 * @property {string} [q]                 标题子串
 * @property {boolean} [include_unpublished]  仅 admin
 */

/**
 * @typedef {Object} ProblemListItem
 * @property {number} id
 * @property {string} title
 * @property {'easy'|'medium'|'hard'} difficulty
 * @property {Tag[]} tags
 * @property {boolean} is_published
 * @property {number} created_by
 * @property {string} created_at
 * @property {{ total: number, accepted: number, pass_rate: number }} stats
 *
 *  注：submission_count / accepted_count 来自 stats.total / stats.accepted
 */

/**
 * @typedef {Object} ProblemListResult
 * @property {ProblemListItem[]} items
 * @property {number} total
 * @property {number} page
 * @property {number} size
 */

/**
 * @param {ProblemListQuery} [q]
 * @returns {Promise<ProblemListResult>}
 */
export function list(q = {}) {
    /** @type {Record<string, any>} */
    const query = {
        page:    q.page    ?? 1,
        size:    q.size    ?? 20,
        sort:    q.sort    ?? 'created_desc',
    };
    if (q.difficulty) query.difficulty = q.difficulty;
    if (q.q)          query.q          = q.q;
    if (q.include_unpublished) query.include_unpublished = '1';
    // tags 数组：后端要求同名参数多次出现 —— 但 client.js 的 buildQuery
    // 对数组只取第一次。手动展开成 sorted 字符串 + 重复键。
    // 这里改为单次传 tags=1,2,3（多选 AND），由后端在 query string 解析
    //   tag=1&tag=2&tag=3  → 数组
    // buildQuery 看到数组只取第一个 —— 改由调用方传 tag1,tag2,tag3 单字段。
    // 取折中：把 tags 数组用 ',' join 成单字符串，由后端按 ',' 切。
    //   (后端实现见 backend/src/http/handlers/problem_handler.cpp)
    if (Array.isArray(q.tags) && q.tags.length > 0) {
        // 后端 (problem_handler.cpp parse_problems_list_query) 接受
        //   1) tag=slug1&tag=slug2&tag=slug3  —— 多次出现
        //   2) tag=slug1,slug2,slug3          —— 逗号分隔
        // URLSearchParams.set() 会覆盖同名键，所以多值数组会被
        // 强转成 "a,b,c" 字符串 —— 走格式 2)，与后端兼容。
        query.tag = q.tags.join(',');
    } else if (typeof q.tag === 'string' && q.tag) {
        query.tag = q.tag;
    }
    return apiGet('/problems', query);
}

// ---------------------------------------------------------------------------
//  详情
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} SampleTestcase
 * @property {number} case_index
 * @property {string} input
 * @property {string} expected_output
 * @property {boolean} is_sample
 * @property {number} score
 */

/**
 * @typedef {Object} ProblemDetailData
 * @property {number} id
 * @property {string} title
 * @property {string} content_md
 * @property {'easy'|'medium'|'hard'} difficulty
 * @property {number} time_limit_ms
 * @property {number} memory_limit_mb
 * @property {number} output_limit_mb
 * @property {Tag[]} tags
 * @property {SampleTestcase[]} sample_testcases
 * @property {boolean} is_published
 * @property {number} created_by
 * @property {string} created_at
 */

/**
 * @param {number|string} id
 * @returns {Promise<ProblemDetailData>}
 */
export function get(id) {
    return apiGet('/problems/' + encodeURIComponent(id));
}

// ---------------------------------------------------------------------------
//  标签列表
// ---------------------------------------------------------------------------

/**
 * @typedef {Object} Tag
 * @property {number} id
 * @property {string} name
 * @property {string} slug
 */

/**
 * @returns {Promise<Tag[]>}
 */
export function tags() {
    return apiGet('/tags');
}
