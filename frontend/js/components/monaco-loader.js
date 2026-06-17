// =============================================================================
//  components/monaco-loader.js — Monaco Editor 加载器 (SPEC §3.3.4 / §3.3.5 H)
//
//  行为：
//    - 首次调用 loadMonaco() 动态插入 <script src="...loader.js">
//    - 同时配置 require.js base 为 monaco vs 文件 CDN 路径
//    - 加载完成 → resolve window.monaco
//    - 失败 / 超时 (10s) → reject
//
//  用法：
//    import { loadMonaco, ensureMonacoLang } from './components/monaco-loader.js';
//    const monaco = await loadMonaco();
//    ensureMonacoLang('cpp');
//    const editor = monaco.editor.create(...);
//
//  Fallback:
//    视图调用 createEditorOrFallback(container, value, lang, onChange) 拿到
//    { kind: 'monaco', editor } 或 { kind: 'textarea', el }，
//    后者用于 Monaco 加载失败的降级。
// =============================================================================

const MONACO_VERSION = '0.45.0';
const MONACO_BASE    = `https://cdn.jsdelivr.net/npm/monaco-editor@${MONACO_VERSION}/min/vs`;
const LOADER_URL     = `${MONACO_BASE}/loader.js`;
const TIMEOUT_MS     = 12000;

let _loading = null;
let _monaco  = null;

/**
 * @typedef {{
 *   id: 'c'|'cpp'|'java'|'python'|'go',
 *   label: string,
 *   monacoLang: string,    // monaco editor 的语言 id
 *   defaultCode: string,
 * }} LangOption
 */

/** SPEC §6.2 / §5.2.3 —— 5 种语言（与后端 submissions.language 枚举对齐） */
export const LANGS = [
    {
        id: 'cpp',
        label: 'C++',
        monacoLang: 'cpp',
        defaultCode:
`#include <bits/stdc++.h>
using namespace std;

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    // TODO
    return 0;
}
`,
    },
    {
        id: 'c',
        label: 'C',
        monacoLang: 'c',
        defaultCode:
`#include <stdio.h>

int main(void) {
    // TODO
    return 0;
}
`,
    },
    {
        id: 'java',
        label: 'Java',
        monacoLang: 'java',
        defaultCode:
`public class Main {
    public static void main(String[] args) {
        // TODO
    }
}
`,
    },
    {
        id: 'python',
        label: 'Python',
        monacoLang: 'python',
        defaultCode:
`def main():
    # TODO
    pass

if __name__ == "__main__":
    main()
`,
    },
    {
        id: 'go',
        label: 'Go',
        monacoLang: 'go',
        defaultCode:
`package main

import "fmt"

func main() {
    // TODO
    _ = fmt.Println
}
`,
    },
];

export const LANG_BY_ID = Object.fromEntries(LANGS.map(l => [l.id, l]));

/**
 * 加载 Monaco（amd loader + 编辑器本体）
 * @returns {Promise<any>} window.monaco
 */
export function loadMonaco() {
    if (_monaco) return Promise.resolve(_monaco);
    if (_loading) return _loading;

    _loading = new Promise((resolve, reject) => {
        if (typeof window === 'undefined') {
            reject(new Error('no window'));
            return;
        }
        // 已被其它模块加载过
        if (window.monaco && window.monaco.editor) {
            _monaco = window.monaco;
            resolve(_monaco);
            return;
        }
        // 配置 require 路径
        if (!window.require) {
            const s = document.createElement('script');
            s.src = LOADER_URL;
            s.async = true;
            s.onload = () => configureAndLoad(resolve, reject);
            s.onerror = () => reject(new Error('failed to load monaco loader.js'));
            document.head.appendChild(s);
        } else {
            configureAndLoad(resolve, reject);
        }

        // 超时保护
        setTimeout(() => {
            if (!_monaco) reject(new Error('monaco load timeout'));
        }, TIMEOUT_MS);
    });

    return _loading;
}

function configureAndLoad(resolve, reject) {
    try {
        const req = window.require;
        req.config({ paths: { vs: MONACO_BASE } });
        // 关闭 telemetry 等不必要的工作
        window.MonacoEnvironment = {
            getWorkerUrl: function () {
                // 简单降级：用 editorWorker（语法高亮），语言 worker 用 proxy
                return 'data:text/javascript;charset=utf-8,' + encodeURIComponent(
                    `self.MonacoEnvironment = { baseUrl: '${MONACO_BASE}/' };
                     importScripts('${MONACO_BASE}/editor/editor.worker.js');`
                );
            }
        };
        req(['vs/editor/editor.main'], () => {
            _monaco = window.monaco;
            resolve(_monaco);
        }, (err) => reject(err));
    } catch (e) {
        reject(e);
    }
}

/**
 * 确保给定 monacoLang 已注册；monaco 自带 cpp/c/java/python/go，无需额外动作，
 * 此函数作为扩展点留出（自定义语言时挂载到此）。
 */
export function ensureMonacoLang(_monacoLang) {
    // monaco-editor 内置：cpp, c, java, python, go —— no-op
    return true;
}

/**
 * 创建 Monaco 实例（带降级 textarea）
 * @param {HTMLElement} container
 * @param {{ value: string, language: string, onChange?: (v: string) => void, readOnly?: boolean }} opts
 * @returns {Promise<{ kind: 'monaco', editor: any, monaco: any, dispose: () => void } | { kind: 'textarea', el: HTMLTextAreaElement, dispose: () => void }>}
 */
export async function createEditorOrFallback(container, opts) {
    try {
        const monaco = await loadMonaco();
        ensureMonacoLang(opts.language);
        const editor = monaco.editor.create(container, {
            value:             opts.value || '',
            language:          opts.language || 'cpp',
            theme:             'vs-dark',
            automaticLayout:   true,
            minimap:           { enabled: false },
            scrollBeyondLastLine: false,
            fontSize:          13,
            tabSize:           4,
            readOnly:          !!opts.readOnly,
            wordWrap:          'off',
        });
        let disp;
        if (opts.onChange) {
            disp = editor.onDidChangeModelContent(() => opts.onChange(editor.getValue()));
        }
        return {
            kind: 'monaco',
            editor,
            monaco,
            dispose: () => {
                try { disp && disp.dispose(); } catch {}
                try { editor.dispose(); } catch {}
            },
        };
    } catch (e) {
        console.warn('[monaco] load failed, fallback to textarea:', e && e.message);
        // Fallback —— 简单 textarea
        const ta = document.createElement('textarea');
        ta.className = 'form-textarea monaco-fallback';
        ta.value     = opts.value || '';
        ta.spellcheck = false;
        ta.style.cssText = 'min-height: 360px; font-family: var(--font-mono); font-size: 13px; line-height: 1.55; tab-size: 4;';
        if (opts.readOnly) ta.readOnly = true;
        container.appendChild(ta);
        if (opts.onChange) {
            ta.addEventListener('input', () => opts.onChange(ta.value));
        }
        return {
            kind: 'textarea',
            el: ta,
            dispose: () => { ta.remove(); },
        };
    }
}
