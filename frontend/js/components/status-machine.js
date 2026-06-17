// =============================================================================
//  components/status-machine.js — 判题状态机可视化 (SPEC §2.3.2)
//
//  判题状态机（4 态主流程 + 8 态终态）：
//
//      queued ──> compiling ──> running ──> finished (one of 8 terminal states)
//                                    │
//                                    └──> finished (early-exit on CE/SE)
//
//  可视化策略：
//    1) 主流程：横向时间线 3 个活跃节点 (queued / compiling / running)
//       · 已完成 = 实心圆 + 绿勾
//       · 当前 = 高亮 + 脉动动画
//       · 未到达 = 灰圈
//    2) 终态：判完 (status=finished) 后，弹出一个"结果"徽章 + 解释条
//    3) 等待中：显示 elapsed timer（自提交起累计秒数）
//
//  API：
//    import { createStatusMachine, updateStatusMachine }
//    const el = createStatusMachine();       // 初始 = queued
//    updateStatusMachine(el, { status: 'compiling', ... });
//    updateStatusMachine(el, { status: 'finished', result: 'WA', ... });
//
//  纯函数 + DOM 操作；不依赖任何全局 store，便于单元测试。
// =============================================================================

import { createEl } from '../utils/dom.js';
import { relativeTime, formatTime, formatMemory } from '../utils/format.js';
import { statusBadge } from '../utils/dom.js';

// ---------------------------------------------------------------------------
//  状态机元数据（与 SPEC §2.3.2 对齐）
// ---------------------------------------------------------------------------

/**
 * 主流程 3 步（queued → compiling → running），early-exit 阶段会跳到 finished。
 * SPEC §2.3.2 文字："queued → compiling → running → finished"
 */
export const MAIN_STEPS = [
    { id: 'queued',    label: '排队',   hint: '等待判题机空闲' },
    { id: 'compiling', label: '编译',   hint: '编译源代码为可执行文件' },
    { id: 'running',   label: '运行',   hint: '逐个测试点运行并比对' },
];

/** 8 态终态（SPEC §2.3.2 / §3.3.5 J） */
export const TERMINAL_RESULTS = ['AC', 'WA', 'TLE', 'MLE', 'OLE', 'RE', 'CE', 'SE'];

/** 8 态中文释义（用于结果区文案） */
export const RESULT_HINT = {
    AC:  '所有测试点通过',
    WA:  '至少一个测试点输出与预期不符',
    TLE: '运行时间超过题目的时间限制',
    MLE: '运行内存超过题目的内存限制',
    OLE: '输出字节数超过题目的输出限制',
    RE:  '进程非 0 退出或被信号终止',
    CE:  '编译阶段失败',
    SE:  '判题机系统级异常',
};

/**
 * 根据当前 status 算出主流程节点的"状态"：
 *   - 'done'   : 已经经过
 *   - 'active' : 当前所在
 *   - 'pending': 还没到
 *   - 'skipped': 早退出（CE/SE 不会真正 running；标灰跳过）
 *
 * 算法：
 *   1) status === 'queued'           → queued=active, 其余=pending
 *   2) status === 'compiling'        → queued=done, compiling=active, running=pending
 *   3) status === 'running'          → queued=done, compiling=done, running=active
 *   4) status === 'finished' + AC/WA/TLE/MLE/OLE/RE → 全 done
 *      status === 'finished' + CE/SE                 → queued=done, compiling=done, running=skipped
 */
export function computeStepStates(status, result) {
    if (status === 'queued') {
        return { queued: 'active', compiling: 'pending', running: 'pending' };
    }
    if (status === 'compiling') {
        return { queued: 'done', compiling: 'active', running: 'pending' };
    }
    if (status === 'running') {
        return { queued: 'done', compiling: 'done', running: 'active' };
    }
    if (status === 'finished') {
        const isEarlyExit = (result === 'CE' || result === 'SE');
        return {
            queued:    'done',
            compiling: 'done',
            running:   isEarlyExit ? 'skipped' : 'done',
        };
    }
    // 未知 status → 全部 pending（防御性）
    return { queued: 'pending', compiling: 'pending', running: 'pending' };
}

/**
 * 主流程图标的"完成度"百分比（用于进度条）：
 *   queued          = 0
 *   compiling       = 50
 *   running         = 75
 *   finished(AC...) = 100
 *   finished(CE/SE) = 66 (compiling done + running skipped)
 */
export function computeProgressPct(status, result) {
    if (status === 'queued')    return 0;
    if (status === 'compiling') return 50;
    if (status === 'running')   return 75;
    if (status === 'finished') {
        if (result === 'CE' || result === 'SE') return 66;
        return 100;
    }
    return 0;
}

// ---------------------------------------------------------------------------
//  组件入口
// ---------------------------------------------------------------------------

/**
 * 创建初始状态机节点（status=queued）。
 * @returns {HTMLElement}
 */
export function createStatusMachine() {
    const root = createEl('div', { class: 'sm', 'data-status': 'queued' });

    // 上：主流程时间线
    root.appendChild(renderTimeline('queued', undefined));

    // 中：进度条
    root.appendChild(renderProgress(0));

    // 下：终态 / 状态说明
    root.appendChild(renderFooter('queued', undefined, null));

    return root;
}

/**
 * 增量更新：data 变化时尽量复用 DOM 节点，只改 className / textContent。
 * @param {HTMLElement} root      createStatusMachine() 返回的根节点
 * @param {{ status: string, result?: string|null, time_used_ms?: number, memory_used_kb?: number, created_at?: string }} data
 */
export function updateStatusMachine(root, data) {
    if (!root || !root.classList.contains('sm')) return;
    const status = (data && data.status) || 'queued';
    const result = data && data.result;
    root.dataset.status = status;
    if (result) root.dataset.result = result;
    else delete root.dataset.result;

    // 1) timeline
    const tl = root.querySelector('.sm-timeline');
    if (tl) {
        const next = renderTimeline(status, result);
        tl.replaceWith(next);
    }

    // 2) progress
    const pb = root.querySelector('.sm-progress');
    if (pb) {
        const pct = computeProgressPct(status, result);
        const next = renderProgress(pct);
        pb.replaceWith(next);
    }

    // 3) footer
    const ft = root.querySelector('.sm-footer');
    if (ft) {
        const next = renderFooter(status, result, data);
        ft.replaceWith(next);
    }
}

// ---------------------------------------------------------------------------
//  子视图
// ---------------------------------------------------------------------------

function renderTimeline(status, result) {
    const states = computeStepStates(status, result);
    const wrap = createEl('div', { class: 'sm-timeline' });
    MAIN_STEPS.forEach((step, i) => {
        const node = createEl('div', { class: 'sm-node', 'data-step': step.id, 'data-state': states[step.id] });
        const dot  = createEl('div', { class: 'sm-node__dot' });
        const lbl  = createEl('div', { class: 'sm-node__label' }, step.label);
        const hint = createEl('div', { class: 'sm-node__hint muted' }, step.hint);
        node.appendChild(dot);
        node.appendChild(lbl);
        node.appendChild(hint);
        wrap.appendChild(node);

        // 连线（非最后一步）
        if (i < MAIN_STEPS.length - 1) {
            const link = createEl('div', { class: 'sm-link' });
            // link done: 前一步 done 时才点亮
            const prevState = states[MAIN_STEPS[i].id];
            link.dataset.state = prevState === 'done' ? 'done'
                : (prevState === 'active' ? 'active' : 'pending');
            wrap.appendChild(link);
        }
    });
    return wrap;
}

function renderProgress(pct) {
    const wrap = createEl('div', { class: 'sm-progress' });
    const bar  = createEl('div', { class: 'sm-progress__bar' });
    bar.style.width = pct + '%';
    bar.dataset.pct = String(pct);
    wrap.appendChild(bar);
    return wrap;
}

function renderFooter(status, result, data) {
    const ft = createEl('div', { class: 'sm-footer' });

    if (status !== 'finished') {
        // 判题中：显示 elapsed + 当前状态文案
        const elapsed = data && data.created_at ? relativeTime(data.created_at) : '—';
        const cur = MAIN_STEPS.find(s => s.id === status) || MAIN_STEPS[0];
        ft.appendChild(createEl('div', { class: 'sm-footer__state' }, [
            createEl('span', { class: 'muted' }, '当前：'),
            createEl('span', { class: 'sm-footer__state-name' }, cur.label),
        ]));
        ft.appendChild(createEl('div', { class: 'sm-footer__elapsed muted' }, [
            '已等待 ',
            createEl('span', null, elapsed),
        ]));
        return ft;
    }

    // status=finished：显示结果徽章 + 中文释义 + 耗时/内存
    const hint = RESULT_HINT[result] || '判题完成';
    ft.appendChild(createEl('div', { class: 'sm-footer__result' }, [
        statusBadge(result || 'SE'),
        createEl('span', { class: 'sm-footer__result-hint' }, hint),
    ]));
    if (data) {
        const metrics = createEl('div', { class: 'sm-footer__metrics muted' });
        if (Number.isFinite(data.time_used_ms)) {
            metrics.appendChild(createEl('span', null, [
                '耗时 ', createEl('b', null, formatTime(data.time_used_ms)),
            ]));
        }
        if (Number.isFinite(data.memory_used_kb)) {
            metrics.appendChild(createEl('span', null, [
                ' · 内存 ', createEl('b', null, formatMemory(data.memory_used_kb)),
            ]));
        }
        ft.appendChild(metrics);
    }
    return ft;
}
