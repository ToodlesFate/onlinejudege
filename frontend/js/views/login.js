// =============================================================================
//  views/login.js — 登录页 (SPEC §3.3.5 E)
//
//  行为：
//    - 居中卡片（宽 400px）
//    - 字段：username, password
//    - 提交时按钮 disabled + "登录中..."
//    - 成功 → 跳 ?redirect= 参数或 /
//    - 失败 → Toast 显示 message；表单内联错误也保留一行
//    - 客户端校验：username / password 非空、password ≥ 8
// =============================================================================

import { createEl } from '../utils/dom.js';
import { login } from '../api/auth.js';
import { toast } from '../components/toast.js';
import { ApiError, HttpError } from '../api/client.js';

const USERNAME_RE = /^[A-Za-z0-9_]{3,20}$/;

export default async function loginView(_params, query) {
    const redirect = (query && query.get('redirect')) || '/';

    const root = createEl('div', { class: 'view container' });

    const card = createEl('div', { class: 'auth-card' });
    card.appendChild(createEl('h1', { class: 'auth-card__title' }, '登录'));
    card.appendChild(createEl('p',  { class: 'auth-card__subtitle muted' },
        '使用用户名和密码登录你的 OnlineJudge 账号。'));

    // ---- form ----
    const form = createEl('form', { class: 'auth-form', noValidate: true, autocomplete: 'on' });
    form.appendChild(formGroup({
        id: 'login-username',
        label: '用户名',
        type: 'text',
        name: 'username',
        autocomplete: 'username',
        placeholder: '3-20 字符，字母/数字/下划线',
    }));
    form.appendChild(formGroup({
        id: 'login-password',
        label: '密码',
        type: 'password',
        name: 'password',
        autocomplete: 'current-password',
        placeholder: '至少 8 字符',
    }));

    const errorEl = createEl('div', { class: 'form-error auth-form__error', role: 'alert' });
    form.appendChild(errorEl);

    const submitBtn = createEl('button', {
        class: 'btn btn--primary btn--block',
        type: 'submit',
    }, '登 录');
    form.appendChild(submitBtn);

    card.appendChild(form);

    // 底部跳转
    card.appendChild(createEl('div', { class: 'auth-card__foot' }, [
        createEl('span', { class: 'muted' }, '还没有账号？'),
        createEl('a', { href: '/register' + (redirect !== '/' ? '?redirect=' + encodeURIComponent(redirect) : '') },
                 '立即注册'),
    ]));

    root.appendChild(createEl('div', { class: 'auth-wrap' }, [card]));

    // ---- 校验 & 提交 ----
    function setError(msg) {
        errorEl.textContent = msg || '';
        errorEl.style.display = msg ? 'block' : 'none';
    }

    function validate({ username, password }) {
        if (!username) return '请输入用户名';
        if (!USERNAME_RE.test(username)) return '用户名格式：3-20 字符，仅字母/数字/下划线';
        if (!password) return '请输入密码';
        if (password.length < 8) return '密码至少 8 字符';
        return null;
    }

    async function onSubmit(e) {
        e.preventDefault();
        setError('');

        const fd = new FormData(form);
        const payload = {
            username: String(fd.get('username') || '').trim(),
            password: String(fd.get('password') || ''),
        };

        const v = validate(payload);
        if (v) { setError(v); return; }

        submitBtn.disabled = true;
        const oldText = submitBtn.textContent;
        submitBtn.textContent = '登录中…';
        try {
            const user = await login(payload);
            toast(`欢迎回来，${user.username || payload.username}`, 'success');
            // 跳 redirect —— 用 location.assign 强制刷新一次 router 状态
            // （header 中的订阅是同一个 authStore，但 router 路由是按需懒挂载，
            //  重渲由 popstate 触发即可）
            window.location.assign(redirect);
        } catch (err) {
            const msg = mapErrMsg(err);
            setError(msg);
            toast(msg, 'error');
            submitBtn.disabled = false;
            submitBtn.textContent = oldText;
        }
    }

    form.addEventListener('submit', onSubmit);

    return root;
}

function formGroup({ id, label, type, name, autocomplete, placeholder }) {
    const wrap = createEl('div', { class: 'form-group' });
    const lbl  = createEl('label', { class: 'form-label', for: id }, [
        label,
        createEl('span', { class: 'form-label__required' }, '*'),
    ]);
    const input = createEl('input', {
        class: 'form-input',
        id, name, type, placeholder,
        autocomplete: autocomplete || 'off',
        required: true,
    });
    wrap.appendChild(lbl);
    wrap.appendChild(input);
    return wrap;
}

function mapErrMsg(err) {
    if (err instanceof ApiError) {
        if (err.code === 1002) return '用户名或密码错误';
        if (err.code === 1001) return err.message || '请求参数有误';
        if (err.code === 1008) return '服务暂时不可用，请稍后再试';
        return err.message || `登录失败 (code=${err.code})`;
    }
    if (err instanceof HttpError) {
        if (err.status === 0)  return '网络错误，请检查连接';
        if (err.status >= 500) return '服务异常，请稍后再试';
        return `请求失败 (HTTP ${err.status})`;
    }
    return '登录失败：' + (err && err.message || err);
}
