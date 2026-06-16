// =============================================================================
//  views/_stub.js — 尚未实现的视图统一占位
//
//  Phase 1 任务只要求"前端骨架"；除首页与 404 外的视图
//  （登录 / 注册 / 题目列表 / 题目详情 / 提交列表 / ...）
//  在对应 Phase 落地之前用本占位文件生成一个友好的"敬请期待"页，
//  既验证路由表注册无误，也避免在 Phase 1 阶段就给用户假数据。
// =============================================================================

import { createEl, escapeHtml } from '../utils/dom.js';

/**
 * 构造一个占位视图。
 *
 * @param {string} title         页面标题
 * @param {string} phase         该视图计划落地的 Phase 编号
 * @param {string} [hint]        附加说明
 * @returns {(params: Record<string,string>) => HTMLElement}
 */
export function stubView(title, phase, hint) {
    return async function (params) {
        const paramsLine = params && Object.keys(params).length
            ? `当前路径参数：${Object.entries(params)
                .map(([k, v]) => `${k} = ${escapeHtml(v)}`).join('，')}`
            : '';

        return createEl('div', { class: 'view container' }, [
            createEl('div', { class: 'view__header' }, [
                createEl('div', null, [
                    createEl('h1', { class: 'view__title' }, title),
                    createEl('p',  { class: 'view__subtitle' },
                             `此页面将在 Phase ${phase} 落地 — 当前为前端骨架占位。`),
                ]),
                createEl('div', { class: 'view__actions' }, [
                    createEl('a', { class: 'btn btn--ghost', href: '/' }, '← 返回首页'),
                ]),
            ]),
            createEl('div', { class: 'card' }, [
                createEl('div', { class: 'card__title' }, '页面状态'),
                createEl('p',  { class: 'card__desc' },
                    hint || '本视图尚未实现。基础路由、主题、布局与本占位页均已就绪。'),
                paramsLine ? createEl('pre', { class: 'mt-4' }, paramsLine) : null,
            ]),
        ]);
    };
}
