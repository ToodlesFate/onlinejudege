// =============================================================================
//  views/not-found.js — 404 页面 (SPEC §3.3.5 O)
// =============================================================================

import { createEl } from '../utils/dom.js';

export default async function notFoundView() {
    return createEl('div', { class: 'view container' }, [
        createEl('div', { class: 'empty', style: { padding: '96px 0' } }, [
            createEl('div', {
                style: {
                    fontSize: '72px',
                    fontWeight: '700',
                    color: 'var(--fg-3)',
                    lineHeight: '1',
                },
            }, '404'),
            createEl('div', { class: 'empty__title' }, '页面不存在'),
            createEl('div', { class: 'empty__hint' }, '你访问的路径可能已被移除，或地址输入有误。'),
            createEl('a', {
                class: 'btn btn--primary mt-4',
                href: '/',
            }, '返回首页'),
        ]),
    ]);
}
