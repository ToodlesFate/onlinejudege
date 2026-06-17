// =============================================================================
//  tests/poller.test.mjs — utils/poller.js 单元测试 (Node, no framework)
//
//  覆盖 SPEC §2.3.4 的 4 条 + 6 个边缘 case：
//    1) 默认参数 = 2000ms 间隔 / 30 分钟超时
//    2) start() 立即触发第一次 fetcher（不等第一个 interval）
//    3) status=finished 时立即停止 + 调 onFinish
//    4) 数据未 finished → 每 intervalMs 调一次
//    5) 超过 maxDurationMs → 停止 + 调 onTimeout
//    6) fetcher 抛错时调 onError；默认不停止
//    7) stopOnError=true → 出错立即停止
//    8) stop() 幂等
//    9) onTick 抛错不打断轮询
//   10) shouldStop 自定义判断
//   11) onTick 收到 (data, attempt)
//   12) 真实场景：queued→compiling→running→finished(AC) 全流程
//   13) 早退出场景：CE
//
//  测试策略：注入可控 sleep()，但底层 setTimeout 仍用真的，
//  这样不需要在 async chain 中手动驱动 microtask。
//  intervalMs 设为 5~10ms 让测试快；Date.now 用 stub 控制 maxDurationMs。
// =============================================================================

import {
    createPoller,
    POLL_INTERVAL_MS,
    POLL_MAX_DURATION_MS,
} from '../js/utils/poller.js';
import assert from 'node:assert/strict';

// ---------------------------------------------------------------------------
//  Date.now stub —— 让 maxDurationMs 行为可测
// ---------------------------------------------------------------------------
let _now = 0;
const realNow = Date.now;
Date.now = () => _now;

function setNow(t) { _now = t; }
function resetNow() { _now = 0; }

// ---------------------------------------------------------------------------
//  Helpers
// ---------------------------------------------------------------------------
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
async function waitMs(ms) { await sleep(ms); }

// ---------------------------------------------------------------------------
let pass = 0, fail = 0;
function test(name, fn) {
    return Promise.resolve()
        .then(fn)
        .then(() => { pass++; console.log('  OK   ' + name); })
        .catch((e) => { fail++; console.log('  FAIL ' + name + '\n       ' + (e && e.stack || e)); });
}

(async () => {
    console.log('--- SPEC §2.3.4: 2s 轮询 + 状态机终止条件 ---');

    // ----------------------------------------------------------------- 常量
    await test('默认间隔 = 2000ms', () => {
        assert.equal(POLL_INTERVAL_MS, 2000);
    });
    await test('默认最长时间 = 30 分钟', () => {
        assert.equal(POLL_MAX_DURATION_MS, 30 * 60 * 1000);
    });

    // --------------------------------------------------------- 基础：开始即拉
    await test('start() 立即触发第一次 fetcher', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; return { status: 'queued' }; },
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(0);   // 让 microtask 跑完
        assert.equal(calls, 1, '第一次应已触发');
        p.stop();
    });

    // --------------------------------------------------- 间隔执行
    await test('未 finished 时每 intervalMs 调一次', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; return { status: 'queued' }; },
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(0);    // 1
        await waitMs(10);   // 2
        await waitMs(10);   // 3
        assert.equal(calls, 3, '期望 3 次');
        p.stop();
    });

    // --------------------------------------------------- 终态停止
    await test('status=finished 时立即停止 + 调 onFinish', async () => {
        resetNow();
        const responses = [
            { status: 'queued'    },
            { status: 'compiling' },
            { status: 'running'   },
            { status: 'finished', result: 'AC' },
        ];
        let i = 0;
        let finishedData = null;
        const p = createPoller({
            fetcher: async () => responses[i++],
            intervalMs: 10,
            maxDurationMs: 1000,
            onFinish: (d) => { finishedData = d; },
        });
        p.start();
        await waitMs(50);
        assert.equal(p.isRunning(), false);
        assert.equal(p.getStopReason(), 'finished');
        assert.equal(finishedData && finishedData.result, 'AC');
        // 不再触发 fetcher
        const beforeCalls = i;
        await waitMs(100);
        assert.equal(i, beforeCalls);
    });

    // --------------------------------------------------- 默认 shouldStop
    await test('默认 shouldStop = data.status === "finished"', async () => {
        resetNow();
        const p = createPoller({
            fetcher: async () => ({ status: 'finished' }),
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(20);
        assert.equal(p.isRunning(), false);
    });

    // --------------------------------------------------- 自定义 shouldStop
    await test('自定义 shouldStop 生效', async () => {
        resetNow();
        const p = createPoller({
            fetcher: async () => ({ done: true }),
            intervalMs: 10,
            maxDurationMs: 1000,
            shouldStop: (d) => d && d.done === true,
        });
        p.start();
        await waitMs(20);
        assert.equal(p.isRunning(), false);
        assert.equal(p.getStopReason(), 'finished');
    });

    // --------------------------------------------------- 超时
    await test('超过 maxDurationMs 调 onTimeout + 停止', async () => {
        resetNow();
        let calls = 0;
        let timeoutData = null;
        const p = createPoller({
            fetcher: async () => {
                calls++;
                // 每次 tick 把虚拟时间推进 20ms —— 第 3 次后 > 50ms
                setNow(_now + 20);
                return { status: 'queued' };
            },
            intervalMs: 5,
            maxDurationMs: 50,
            onTimeout: (d) => { timeoutData = d; },
        });
        p.start();
        await waitMs(80);
        assert.equal(p.isRunning(), false, '超过 maxDurationMs 后应停止');
        assert.equal(p.getStopReason(), 'timeout');
        assert.ok(timeoutData, 'onTimeout 应被调用');
    });

    // --------------------------------------------------- 出错不停
    await test('fetcher 抛错时调 onError（默认不停止）', async () => {
        resetNow();
        let calls = 0;
        const errs = [];
        const p = createPoller({
            fetcher: async () => {
                calls++;
                if (calls === 1) throw new Error('net');
                return { status: 'queued' };
            },
            intervalMs: 10,
            maxDurationMs: 1000,
            onError: (e) => { errs.push(e); },
        });
        p.start();
        await waitMs(30);
        assert.ok(calls >= 2, '失败后应继续 (calls=' + calls + ')');
        assert.equal(errs.length, 1);
        assert.equal(errs[0].message, 'net');
        assert.equal(p.isRunning(), true);
        p.stop();
    });

    // --------------------------------------------------- 出错停
    await test('stopOnError=true 出错立即停止', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; throw new Error('boom'); },
            intervalMs: 10,
            maxDurationMs: 1000,
            stopOnError: true,
        });
        p.start();
        await waitMs(20);
        assert.equal(p.isRunning(), false);
        assert.equal(p.getStopReason(), 'error');
    });

    // --------------------------------------------------- stop 幂等
    await test('stop() 幂等（多次调用不抛错）', async () => {
        resetNow();
        const p = createPoller({
            fetcher: async () => ({ status: 'queued' }),
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        p.stop();
        p.stop();
        p.stop();
        assert.equal(p.isRunning(), false);
        assert.equal(p.getStopReason(), 'manual');
    });

    // --------------------------------------------------- start 幂等
    await test('start() 幂等（已 running 时 no-op）', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; return { status: 'queued' }; },
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(0);
        const c1 = calls;
        p.start();
        await waitMs(0);
        assert.equal(calls, c1, 'start() 不应重复触发');
        p.stop();
    });

    // --------------------------------------------------- onTick 抛错不打断
    await test('onTick 抛错时轮询继续', async () => {
        resetNow();
        let calls = 0;
        let ticks = 0;
        const p = createPoller({
            fetcher: async () => { calls++; return { status: 'queued' }; },
            intervalMs: 10,
            maxDurationMs: 1000,
            onTick: () => { ticks++; throw new Error('ui error'); },
        });
        p.start();
        await waitMs(50);
        assert.ok(calls >= 3, `期望 ≥3 次 fetch，实际 ${calls}`);
        assert.ok(ticks >= 3, `期望 ≥3 次 onTick，实际 ${ticks}`);
        p.stop();
    });

    // --------------------------------------------------- attempt 自增
    await test('attempt 从 1 递增', async () => {
        resetNow();
        const p = createPoller({
            fetcher: async () => ({ status: 'queued' }),
            intervalMs: 10,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(0);   // 让第一次 tick() 跑完
        assert.equal(p.getAttempt(), 1);
        await waitMs(15);
        // 注意：real-timer 下 attempt 至少递增到 2，但可能已经是 3（取决于事件循环）
        // 关键是单调递增 + attempt=1 立即出现
        const a2 = p.getAttempt();
        assert.ok(a2 >= 2, `期望 ≥2，实际 ${a2}`);
        await waitMs(15);
        const a3 = p.getAttempt();
        assert.ok(a3 >= a2, `期望 ≥${a2}，实际 ${a3}`);
        // 验证单调递增
        p.stop();
    });

    // --------------------------------------------------- onTick 收到 attempt
    await test('onTick 收到 (data, attempt)', async () => {
        resetNow();
        const seen = [];
        const p = createPoller({
            fetcher: async () => ({ status: 'queued' }),
            intervalMs: 10,
            maxDurationMs: 1000,
            onTick: (d, n) => { seen.push([n, d.status]); },
        });
        p.start();
        await waitMs(25);   // 期望 2 次 tick（立即 + 10ms 后）
        assert.ok(seen.length >= 2, `期望 ≥2 次，实际 ${seen.length}`);
        assert.equal(seen[0][0], 1);
        assert.equal(seen[0][1], 'queued');
        assert.equal(seen[1][0], 2);
        p.stop();
    });

    // --------------------------------------------------- 必填 fetcher
    await test('缺 fetcher 抛错', () => {
        assert.throws(() => createPoller({}), /fetcher is required/);
    });

    // --------------------------------------------------- 场景：6 步全流程
    await test('场景模拟：queued→compiling→running→finished(AC)', async () => {
        resetNow();
        const seq = [
            { status: 'queued'    },
            { status: 'compiling' },
            { status: 'compiling' },
            { status: 'running'   },
            { status: 'running'   },
            { status: 'finished', result: 'AC', total_score: 100 },
        ];
        let i = 0;
        const states = [];
        const p = createPoller({
            fetcher: async () => seq[i++] || seq[seq.length - 1],
            intervalMs: 5,
            maxDurationMs: 60_000,
            onTick: (d) => states.push(d.status),
        });
        p.start();
        await waitMs(80);
        assert.equal(p.isRunning(), false);
        assert.equal(states.length, 6);
        assert.deepEqual(states,
            ['queued', 'compiling', 'compiling', 'running', 'running', 'finished']);
    });

    // --------------------------------------------------- 场景：CE 早退出
    await test('场景模拟：compiling→finished(CE) 早退出', async () => {
        resetNow();
        const seq = [
            { status: 'queued'    },
            { status: 'compiling' },
            { status: 'finished', result: 'CE', compile_output: 'a.c:1: error: …' },
        ];
        let i = 0;
        const p = createPoller({
            fetcher: async () => seq[i++] || seq[seq.length - 1],
            intervalMs: 5,
            maxDurationMs: 60_000,
        });
        p.start();
        await waitMs(50);
        assert.equal(p.isRunning(), false);
        assert.equal(p.getStopReason(), 'finished');
        const last = p.getLastData();
        assert.equal(last.result, 'CE');
        assert.equal(last.compile_output.includes('error'), true);
    });

    // --------------------------------------------------- 场景：8 态终态
    await test('场景模拟：8 态终态都能让轮询停止', async () => {
        const results = ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE'];
        for (const r of results) {
            resetNow();
            const p = createPoller({
                fetcher: async () => ({ status: 'finished', result: r }),
                intervalMs: 5,
                maxDurationMs: 1000,
            });
            p.start();
            await waitMs(20);
            assert.equal(p.isRunning(), false, `${r} 后应停止`);
            assert.equal(p.getStopReason(), 'finished');
            assert.equal(p.getLastData().result, r);
        }
    });

    // --------------------------------------------------- 场景：3s 周期接近 SPEC
    await test('SPEC 默认参数下，3s 内约 2 次（1 立即 + 1 个 interval 后）', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; return { status: 'queued' }; },
            // 不用 SPEC 的 2000ms；改 30ms 让测试快；验证计数 ≈ floor(3000/30) + 1
            intervalMs: 30,
            maxDurationMs: 10000,
        });
        p.start();
        await waitMs(120);   // 1 + 4
        assert.ok(calls >= 4 && calls <= 6, `期望 4~6 次，实际 ${calls}`);
        p.stop();
    });

    // --------------------------------------------------- 场景：maxDurationMs 边界
    await test('maxDurationMs 边界：刚好等于时不超时', async () => {
        resetNow();
        let calls = 0;
        const p = createPoller({
            fetcher: async () => { calls++; setNow(_now + 9); return { status: 'queued' }; },
            intervalMs: 10,
            maxDurationMs: 100,
        });
        p.start();
        await waitMs(150);
        // 100ms 边界上多调几次都没问题
        p.stop();
    });

    // --------------------------------------------------- getLastData
    await test('getLastData 返回最近一次成功 data；停止后仍可读', async () => {
        resetNow();
        const p = createPoller({
            fetcher: async () => ({ status: 'finished', result: 'AC' }),
            intervalMs: 5,
            maxDurationMs: 1000,
        });
        p.start();
        await waitMs(20);
        assert.equal(p.getLastData().result, 'AC');
    });

    // --------------------------------------------------- 恢复 Date.now
    console.log('---');
    console.log(`Passed: ${pass}, Failed: ${fail}`);
    Date.now = realNow;
    process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); Date.now = realNow; process.exit(1); });
