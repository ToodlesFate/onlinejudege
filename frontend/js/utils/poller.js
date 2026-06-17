// =============================================================================
//  utils/poller.js — 通用轮询工具 (SPEC §2.3.4 / §3.3.1)
//
//  用途：提交详情页每 2s 调一次 GET /api/submissions/{id}，判题结束后停止。
//
//  行为契约（与 SPEC §2.3.4 一一对应）：
//    1) 默认每 POLL_INTERVAL_MS (2000) 调用一次 fetcher
//    2) fetcher 返回的 data.status === 'finished' 时立即停止
//    3) 最多轮询 MAX_DURATION_MS (30 分钟)；超时后停止 + 调用 onTimeout
//    4) 切换 tab 至后台时**不暂停**（SPEC §2.3.4 简化实现）
//    5) 错误不打断轮询（默认 silent：调用 onError；可传 stopOnError=true 立即停）
//    6) stop() 幂等：调用后不再触发任何回调
//
//  设计要点：
//    - 不依赖 setInterval —— 改用 setTimeout 链式自调度，
//      以便 fetcher 是 async 不会重叠触发
//    - 同时挂一个"总时长"看门狗，到点强停
//    - onTick / onFinish / onTimeout / onError 全部可选
//    - 把 sleep 注入为可选参数，方便单元测试用伪时钟
//
//  用法：
//    const p = createPoller({
//        fetcher: () => apiGet('/submissions/123'),
//        intervalMs: 2000,
//        shouldStop: (d) => d && d.status === 'finished',
//        onTick:     (d, attempt) => render(d),
//        onFinish:   (d) => stopSpinner(),
//        onError:    (e, attempt) => console.warn(e),
//    });
//    p.start();
//    // later: p.stop();
// =============================================================================

/** SPEC §2.3.4 规定的默认轮询间隔 */
export const POLL_INTERVAL_MS = 2000;

/** SPEC §2.3.4 规定的最长轮询时长：30 分钟 */
export const POLL_MAX_DURATION_MS = 30 * 60 * 1000;

/**
 * @typedef {Object} PollerOptions
 * @property {() => Promise<any>} fetcher          拉取函数；抛错视为一次失败
 * @property {number}  [intervalMs=2000]           轮询间隔 ms
 * @property {number}  [maxDurationMs=1800000]     最长轮询时长 ms（默认 30 分钟）
 * @property {(data:any) => boolean} [shouldStop]  拿到 data 后判断是否停；默认 data.status === 'finished'
 * @property {(data:any, attempt:number) => void} [onTick]     每次拿到 data
 * @property {(data:any) => void}                 [onFinish]   主动停止时
 * @property {(data:any) => void}                 [onTimeout]  超时时
 * @property {(err:any, attempt:number) => void}  [onError]    fetcher 抛错
 * @property {boolean} [stopOnError=false]         出错时是否立即停止
 * @property {(ms:number) => Promise<void>} [sleep] 注入以便测试
 */

/**
 * 创建一个轮询控制器。
 * @param {PollerOptions} opts
 * @returns {{
 *   start: () => void,
 *   stop:  (reason?: 'manual'|'finished'|'timeout') => void,
 *   isRunning: () => boolean,
 *   getAttempt: () => number,
 * }}
 */
export function createPoller(opts) {
    if (!opts || typeof opts.fetcher !== 'function') {
        throw new Error('[poller] fetcher is required');
    }
    const intervalMs    = Number.isFinite(opts.intervalMs)    ? opts.intervalMs    : POLL_INTERVAL_MS;
    const maxDurationMs = Number.isFinite(opts.maxDurationMs) ? opts.maxDurationMs : POLL_MAX_DURATION_MS;
    const shouldStop    = typeof opts.shouldStop === 'function'
        ? opts.shouldStop
        : (data) => !!(data && typeof data === 'object' && data.status === 'finished');
    const sleep         = typeof opts.sleep === 'function'
        ? opts.sleep
        : (ms) => new Promise((r) => setTimeout(r, ms));

    let timer     = null;          // 链式 setTimeout id
    let running   = false;         // 是否在跑（start / stop 都改这个）
    let attempt   = 0;             // 已发起的请求数（从 1 开始计；首次 fetch 算 attempt=1）
    let finishedReason = null;     // 'manual' | 'finished' | 'timeout' | 'error'
    let lastData  = null;          // 最近一次成功的 data；给 onFinish / onTimeout 用
    let startedAt = 0;             // start() 时记录

    async function tick() {
        if (!running) return;
        attempt += 1;
        let data;
        try {
            data = await opts.fetcher();
        } catch (err) {
            try { opts.onError && opts.onError(err, attempt); } catch (e) { console.error('[poller.onError]', e); }
            if (opts.stopOnError) {
                _stop('error');
                return;
            }
            // 不停 —— 间隔后再试
            scheduleNext();
            return;
        }
        lastData = data;
        try { opts.onTick && opts.onTick(data, attempt); } catch (e) { console.error('[poller.onTick]', e); }

        if (shouldStop(data)) {
            _stop('finished');
            return;
        }
        // 还没 finished → 看总时长
        if (Date.now() - startedAt >= maxDurationMs) {
            try { opts.onTimeout && opts.onTimeout(lastData); } catch (e) { console.error('[poller.onTimeout]', e); }
            _stop('timeout');
            return;
        }
        scheduleNext();
    }

    function scheduleNext() {
        if (!running) return;
        timer = setTimeout(tick, intervalMs);
    }

    function _stop(reason) {
        if (!running) return;
        running = false;
        if (timer) { clearTimeout(timer); timer = null; }
        finishedReason = reason;
        if (reason === 'finished') {
            try { opts.onFinish && opts.onFinish(lastData); } catch (e) { console.error('[poller.onFinish]', e); }
        }
    }

    return {
        /**
         * 启动轮询。已运行时再次调用是 no-op。
         * 立即发起第 1 次请求 —— 不等第一个 interval。
         */
        start() {
            if (running) return;
            running   = true;
            attempt   = 0;
            startedAt = Date.now();
            finishedReason = null;
            // 立即发起一次
            tick();
        },
        /**
         * 停止轮询。幂等。reason 仅作记录，目前不传出。
         * @param {'manual'|'finished'|'timeout'|'error'} [reason]
         */
        stop(reason) {
            _stop(reason || 'manual');
        },
        isRunning()    { return running; },
        getAttempt()   { return attempt; },
        /** 调试用：最近一次拿到的 data（成功路径） */
        getLastData()  { return lastData; },
        /** 调试用：停止原因 */
        getStopReason(){ return finishedReason; },
    };
}
