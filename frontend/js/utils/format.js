// =============================================================================
//  utils/format.js — 通用格式化 (SPEC §3.3.5 A 配套)
//
//  覆盖：
//    - difficultyZh('easy'/'medium'/'hard') → '易'/'中'/'难'
//    - formatTime(ms)  → '15 ms' / '1.2 s'
//    - formatMemory(kb) → '4 MB' / '1.2 GB'
//    - formatPassRate(pct) → '75%' / '—'
//    - formatDateTime(iso) → '2026-04-23 10:00:00'
//    - relativeTime(iso)   → '3 分钟前' / '刚刚'
//    - escapeCsv(slug)     → '栈%2F队列' (URL 编码)
// =============================================================================

const DIFFICULTY_ZH = { easy: '易', medium: '中', hard: '难' };

/**
 * @param {string} d
 * @returns {string}  '易' | '中' | '难' | 原值
 */
export function difficultyZh(d) {
    if (!d) return '';
    return DIFFICULTY_ZH[d] || d;
}

/**
 * @param {number} ms
 */
export function formatTime(ms) {
    if (ms == null) return '—';
    if (ms < 1000) return `${ms} ms`;
    return `${(ms / 1000).toFixed(2)} s`;
}

/**
 * @param {number} kb
 */
export function formatMemory(kb) {
    if (kb == null) return '—';
    if (kb < 1024) return `${kb} KB`;
    const mb = kb / 1024;
    if (mb < 1024) return `${mb.toFixed(mb < 10 ? 1 : 0)} MB`;
    return `${(mb / 1024).toFixed(2)} GB`;
}

/**
 * @param {number} pct  0..100
 */
export function formatPassRate(pct) {
    if (pct == null) return '—';
    return `${pct}%`;
}

/**
 * @param {string} iso
 */
export function formatDateTime(iso) {
    if (!iso) return '—';
    const d = new Date(iso);
    if (isNaN(d.getTime())) return iso;
    const p = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ` +
           `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

/**
 * 相对时间（中文）—— "刚刚" / "3 分钟前" / "2 小时前" / "5 天前" / 否则绝对时间
 * @param {string} iso
 */
export function relativeTime(iso) {
    if (!iso) return '—';
    const d = new Date(iso);
    if (isNaN(d.getTime())) return iso;
    const diff = Math.floor((Date.now() - d.getTime()) / 1000);
    if (diff < 5)        return '刚刚';
    if (diff < 60)       return `${diff} 秒前`;
    if (diff < 3600)     return `${Math.floor(diff / 60)} 分钟前`;
    if (diff < 86400)    return `${Math.floor(diff / 3600)} 小时前`;
    if (diff < 30 * 86400) return `${Math.floor(diff / 86400)} 天前`;
    return formatDateTime(iso);
}
