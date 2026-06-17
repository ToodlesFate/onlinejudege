// =============================================================================
//  utils/draft.js — 题目代码草稿持久化 (SPEC §3.3.5 H)
//
//  行为：
//    1) 进入页面：load(problemId, lang) 恢复上次未提交的代码
//    2) 编辑器 onChange：debounce 500ms 后 save(problemId, lang, code)
//    3) 切换语言：先 save 当前 lang → 再 load 新 lang
//    4) 提交成功：clear(problemId, lang)
//
//  Key 格式：draft:problem_<id>:<lang>   （SPEC §3.3.5 H 明文规定）
//  存储：localStorage（注入以便测试）
//
//  容错：所有 localStorage 异常（quota / private mode / SecurityError）吞掉
//        退化为"无草稿"，不阻塞页面。
// =============================================================================

const PREFIX = 'draft:problem_';
const SEP    = ':';
const DEBOUNCE_MS = 500;

/**
 * @param {Storage} [storage]  默认 window.localStorage（SSR/测试时注入 mock）
 */
export function makeDraftStore(storage) {
    const store = storage || (typeof window !== 'undefined' ? window.localStorage : null);

    function key(problemId, lang) {
        return PREFIX + problemId + SEP + lang;
    }

    /**
     * 读草稿
     * @param {number|string} problemId
     * @param {string} lang
     * @returns {string} 草稿内容；无则空串
     */
    function load(problemId, lang) {
        if (!store) return '';
        try {
            const v = store.getItem(key(problemId, lang));
            return v == null ? '' : v;
        } catch {
            return '';
        }
    }

    /**
     * 写草稿。空字符串 → 删除 key（避免空白条目占配额）
     * @param {number|string} problemId
     * @param {string} lang
     * @param {string} code
     * @returns {boolean}  是否真正写入了
     */
    function save(problemId, lang, code) {
        if (!store) return false;
        const k = key(problemId, lang);
        try {
            if (code && code.length > 0) {
                store.setItem(k, code);
            } else {
                store.removeItem(k);
            }
            return true;
        } catch {
            return false;
        }
    }

    /**
     * 清掉指定 (problem, lang) 的草稿 —— 用于提交成功 / 重置
     * @param {number|string} problemId
     * @param {string} lang
     */
    function clear(problemId, lang) {
        return save(problemId, lang, '');
    }

    /**
     * 给定 getter（取当前值）+ 调度器；返回 flush / cancel 控制柄。
     * 多个并发的 onChange 共用一个 timer —— 最后一次为准。
     *
     * @param {() => string} getCode
     * @param {(problemId:any, lang:string, code:string) => void} onSave
     * @param {number} [debounceMs]
     */
    function makeScheduler(getCode, onSave, debounceMs) {
        const ms = debounceMs == null ? DEBOUNCE_MS : debounceMs;
        let timer = null;
        /** @type {{problemId:any, lang:string}|null} */
        let lastCtx = null;

        function schedule(problemId, lang) {
            lastCtx = { problemId, lang };
            if (timer) clearTimeout(timer);
            timer = setTimeout(() => {
                timer = null;
                if (!lastCtx) return;
                const code = getCode() || '';
                onSave(lastCtx.problemId, lastCtx.lang, code);
            }, ms);
        }

        function flush() {
            if (timer) { clearTimeout(timer); timer = null; }
            if (!lastCtx) return;
            const code = getCode() || '';
            onSave(lastCtx.problemId, lastCtx.lang, code);
        }

        function cancel() {
            if (timer) { clearTimeout(timer); timer = null; }
            lastCtx = null;
        }

        return { schedule, flush, cancel };
    }

    return { load, save, clear, makeScheduler, key, PREFIX };
}

// 暴露常量供测试与文档
export const DRAFT_PREFIX = PREFIX;
export const DRAFT_DEBOUNCE_MS = DEBOUNCE_MS;
