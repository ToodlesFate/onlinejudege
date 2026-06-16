// =============================================================================
//  views/register.js — 注册页 (SPEC §3.3.5 F)
//
//  行为：
//    - 同登录卡片，多两字段：email、confirm password
//    - 客户端校验：username 3-20 + [A-Za-z0-9_]；email 简单格式；password ≥ 8
//      且含字母+数字；confirm password === password
//    - 提交按钮 disabled 直到所有校验通过（实时监听）
//    - 成功后 → Toast "注册成功，已自动登录" + 跳 /
//
//  说明：
//    SPEC §3.3.5 F 提到"失焦时调 /api/auth/check-username"做实时唯一性校验，
//    但 §5.2.1 后端接口列表中并未列此端点 —— 这里走"提交时由后端 1005
//    Conflict 反馈"的方案，前端不做实时唯一性预检，避免对未实现端点的依赖。
// =============================================================================

import { createEl } from '../utils/dom.js';
import { register } from '../api/auth.js';
import { toast } from '../components/toast.js';
import { ApiError, HttpError } from '../api/client.js';

const USERNAME_RE = /^[A-Za-z0-9_]{3,20}$/;
const EMAIL_RE    = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
const PWD_HAS_LETTER = /[A-Za-z]/;
const PWD_HAS_DIGIT  = /\d/;

export default async function registerView(_params, query) {
    const redirect = (query && query.get('redirect')) || '/';

    const root = createEl('div', { class: 'view container' });

    const card = createEl('div', { class: 'auth-card' });
    card.appendChild(createEl('h1', { class: 'auth-card__title' }, '注册'));
    card.appendChild(createEl('p',  { class: 'auth-card__subtitle muted' },
        '创建你的 OnlineJudge 账号。第一个注册的账号将自动成为管理员。'));

    const form = createEl('form', { class: 'auth-form', noValidate: true, autocomplete: 'on' });
    form.appendChild(formGroup({
        id: 'reg-username', label: '用户名', type: 'text', name: 'username',
        autocomplete: 'username', placeholder: '3-20 字符，字母/数字/下划线',
        hint: '登录用，唯一',
    }));
    form.appendChild(formGroup({
        id: 'reg-email', label: '邮箱', type: 'email', name: 'email',
        autocomplete: 'email', placeholder: '例如 alice@example.com',
    }));
    form.appendChild(formGroup({
        id: 'reg-password', label: '密码', type: 'password', name: 'password',
        autocomplete: 'new-password', placeholder: '≥ 8 字符，含字母和数字',
    }));
    form.appendChild(formGroup({
        id: 'reg-confirm', label: '确认密码', type: 'password', name: 'confirm',
        autocomplete: 'new-password', placeholder: '再输入一次',
    }));

    const errorEl = createEl('div', { class: 'form-error auth-form__error', role: 'alert' });
    form.appendChild(errorEl);

    const submitBtn = createEl('button', {
        class: 'btn btn--primary btn--block',
        type: 'submit',
        disabled: true,
    }, '注册');
    form.appendChild(submitBtn);

    card.appendChild(form);

    card.appendChild(createEl('div', { class: 'auth-card__foot' }, [
        createEl('span', { class: 'muted' }, '已有账号？'),
        createEl('a', { href: '/login' }, '去登录'),
    ]));

    root.appendChild(createEl('div', { class: 'auth-wrap' }, [card]));

    // ---- 引用所有输入 ----
    const uEl = /** @type {HTMLInputElement} */ (form.querySelector('#reg-username'));
    const eEl = /** @type {HTMLInputElement} */ (form.querySelector('#reg-email'));
    const pEl = /** @type {HTMLInputElement} */ (form.querySelector('#reg-password'));
    const cEl = /** @type {HTMLInputElement} */ (form.querySelector('#reg-confirm'));

    // ---- 实时校验 & 按钮 enable ----
    function check() {
        const u = uEl.value.trim();
        const em = eEl.value.trim();
        const p = pEl.value;
        const c = cEl.value;
        const v = validate({ username: u, email: em, password: p, confirm: c });
        submitBtn.disabled = !!v;
        if (v) {
            setFieldError(uEl, !USERNAME_RE.test(u) && u ? 'username' : null);
            setFieldError(eEl, em && !EMAIL_RE.test(em) ? 'email' : null);
            setFieldError(pEl, p && (p.length < 8 || !PWD_HAS_LETTER.test(p) || !PWD_HAS_DIGIT.test(p)) ? 'password' : null);
            setFieldError(cEl, c && c !== p ? 'confirm' : null);
        } else {
            setFieldError(uEl, null);
            setFieldError(eEl, null);
            setFieldError(pEl, null);
            setFieldError(cEl, null);
        }
        return v;
    }
    function setFieldError(input, _kind) {
        if (!input) return;
        input.setAttribute('aria-invalid', _kind ? 'true' : 'false');
    }
    [uEl, eEl, pEl, cEl].forEach(el => el.addEventListener('input', () => { setError(''); check(); }));

    function setError(msg) {
        errorEl.textContent = msg || '';
        errorEl.style.display = msg ? 'block' : 'none';
    }

    async function onSubmit(e) {
        e.preventDefault();
        setError('');

        const payload = {
            username: uEl.value.trim(),
            email:    eEl.value.trim(),
            password: pEl.value,
        };
        const v = check();
        if (v) { setError(v); return; }

        submitBtn.disabled = true;
        const oldText = submitBtn.textContent;
        submitBtn.textContent = '注册中…';
        try {
            const user = await register(payload);
            toast(`注册成功，已自动登录（${user.username || payload.username}）`, 'success');
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

function validate({ username, email, password, confirm }) {
    if (!username) return '请输入用户名';
    if (!USERNAME_RE.test(username)) return '用户名格式：3-20 字符，仅字母/数字/下划线';
    if (!email) return '请输入邮箱';
    if (!EMAIL_RE.test(email)) return '邮箱格式不正确';
    if (!password) return '请输入密码';
    if (password.length < 8) return '密码至少 8 字符';
    if (!PWD_HAS_LETTER.test(password) || !PWD_HAS_DIGIT.test(password))
        return '密码必须同时包含字母和数字';
    if (!confirm) return '请再次输入密码';
    if (confirm !== password) return '两次输入的密码不一致';
    return null;
}

function formGroup({ id, label, type, name, autocomplete, placeholder, hint }) {
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
    if (hint) wrap.appendChild(createEl('div', { class: 'form-hint' }, hint));
    return wrap;
}

function mapErrMsg(err) {
    if (err instanceof ApiError) {
        if (err.code === 1005) {
            // 区分 username / email 冲突
            const m = err.message || '';
            if (m.includes('username')) return '该用户名已被占用';
            if (m.includes('email'))    return '该邮箱已被注册';
            return '用户名或邮箱已被占用';
        }
        if (err.code === 1001) return err.message || '请求参数有误';
        if (err.code === 1008) return '服务暂时不可用，请稍后再试';
        return err.message || `注册失败 (code=${err.code})`;
    }
    if (err instanceof HttpError) {
        if (err.status === 0)  return '网络错误，请检查连接';
        if (err.status >= 500) return '服务异常，请稍后再试';
        return `请求失败 (HTTP ${err.status})`;
    }
    return '注册失败：' + (err && err.message || err);
}
