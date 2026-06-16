// =============================================================================
//  views/home.js — 首页 (SPEC §3.3.5 D)
//  Phase 1 实现：Hero + 3 列特性 + 后端健康检查
//  数据统计区待 Phase 2（用户登录）补全
// =============================================================================

import { createEl } from '../utils/dom.js';

const FEATURES = [
    {
        title: '5 种语言',
        desc:  'C / C++ / Java / Python / Go，统一在 Docker 沙箱中编译与运行。',
        icon:  '⌨',
    },
    {
        title: '自动判分',
        desc:  '提交后秒级出判题结果，逐测试点展示状态、耗时与内存。',
        icon:  '⚡',
    },
    {
        title: '安全沙箱',
        desc:  '无网络 + 只读根文件系统 + 资源硬限，杜绝越狱与提权。',
        icon:  '🛡',
    },
];

export default async function homeView() {
    const root = createEl('div', { class: 'view container' });

    // ------- Hero -------
    root.appendChild(createEl('section', { class: 'hero' }, [
        createEl('h1', { class: 'hero__title' }, 'OnlineJudge'),
        createEl('p',  { class: 'hero__subtitle' },
                 '5 语言 / Docker 沙箱 / 自动评测 — 跑通"注册→做题→提交→AC"端到端流程'),
        createEl('div', { class: 'hero__actions' }, [
            createEl('a', { class: 'btn btn--primary btn--lg', href: '/problems' }, '开始刷题'),
            createEl('a', { class: 'btn btn--secondary btn--lg', href: '/register' }, '注册账号'),
        ]),
    ]));

    // ------- 特性卡 -------
    root.appendChild(createEl('section', { class: 'section' }, [
        createEl('h2', { class: 'section__title' }, '核心能力'),
        createEl('div', { class: 'feature-grid' },
            FEATURES.map(f => createEl('div', { class: 'card card--hover' }, [
                createEl('div', { style: { fontSize: '28px', marginBottom: '8px' } }, f.icon),
                createEl('h3', { class: 'card__title' }, f.title),
                createEl('p',  { class: 'card__desc' },  f.desc),
            ]))
        ),
    ]));

    // ------- 后端联通性自检 -------
    // 用闭包把 healthResult 局部化，避免污染模块作用域
    let healthResult;
    root.appendChild(createEl('section', { class: 'section' }, [
        createEl('h2', { class: 'section__title' }, '后端联通性'),
        createEl('p', { class: 'muted mb-3' },
                 '点击下方按钮调用 GET /api/health，验证 nginx → backend 链路是否通。'),
        createEl('div', {
            style: { display: 'flex', gap: '12px', alignItems: 'center' },
        }, [
            createEl('button', {
                class: 'btn btn--secondary',
                onClick: () => runHealthCheck(healthResult),
            }, '检测后端'),
            createEl('a', { class: 'btn btn--ghost', href: '/login' }, '前往登录 →'),
        ]),
        (healthResult = createEl('pre', { class: 'mt-4' }, '尚未检测')),
    ]));

    return root;
}

/** 调 /api/health，把响应渲染到 target 节点 */
async function runHealthCheck(target) {
    target.textContent = '请求中…';
    const started = performance.now();
    try {
        const res  = await fetch('/api/health', { headers: { 'Accept': 'application/json' } });
        const ms   = Math.round(performance.now() - started);
        const text = await res.text();
        let body;
        try { body = JSON.parse(text); } catch { body = text; }
        target.textContent =
            `HTTP ${res.status}  (${ms} ms)\n` +
            JSON.stringify(body, null, 2);
    } catch (err) {
        target.textContent = '请求失败：' + (err && err.message || err);
    }
}
