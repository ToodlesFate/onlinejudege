// =============================================================================
//  tests/draft.e2e.mjs — 端到端模拟：进入页面 → 编辑 → 切语言 → 离开 → 回来
//
//  流程：
//    1) 模拟"清空"localStorage
//    2) 第一次进入：draft 为空 → load 返回空
//    3) 写入 cpp 草稿 → load 取回
//    4) 切到 java → cpp 草稿应保留；load java 返回空
//    5) 写 java 草稿 → 切回 cpp → 两次草稿都还在
//    6) 模拟"重置"cpp → clear → load 返空；java 不受影响
//    7) 模拟"提交"成功 → clear → load 返空
//
//  全程不依赖 DOM / Monaco —— 只测 draft 模块（视图层已经在 unit test 覆盖）
// =============================================================================

import { makeDraftStore } from '../js/utils/draft.js';
import assert from 'node:assert/strict';

const s = (() => {
    const m = new Map();
    return {
        getItem:    (k)    => m.has(k) ? m.get(k) : null,
        setItem:    (k, v) => m.set(k, String(v)),
        removeItem: (k)    => m.delete(k),
        clear:      ()     => m.clear(),
    };
})();
const d = makeDraftStore(s);

console.log('--- E2E: user journey on /problems/866 ---');

// Step 1: 首次进入
assert.equal(d.load(866, 'cpp'), '', 'step1: 进入页面无草稿');
console.log('  OK step1 首次进入无草稿');

// Step 2: 用户写 C++ 代码 → debounce → save
d.save(866, 'cpp', 'int main(){return 0;}\n');
assert.equal(d.load(866, 'cpp'), 'int main(){return 0;}\n', 'step2: 写完 cpp 草稿');
console.log('  OK step2 写入 cpp 草稿');

// Step 3: 用户切到 Java
d.save(866, 'cpp', d.load(866, 'cpp'));  // 切语言前先存（视图层就是这样做）
d.save(866, 'java', 'class Main { public static void main(String[] a) {} }\n');
assert.equal(d.load(866, 'cpp'),  'int main(){return 0;}\n',          'step3a: cpp 草稿仍存在');
assert.equal(d.load(866, 'java'), 'class Main { public static void main(String[] a) {} }\n', 'step3b: java 草稿写入');
console.log('  OK step3 切到 java 写代码，cpp 草稿未丢');

// Step 4: 切回 cpp
assert.equal(d.load(866, 'cpp'),  'int main(){return 0;}\n', 'step4a: 切回 cpp 恢复 cpp 草稿');
assert.equal(d.load(866, 'java'), 'class Main { public static void main(String[] a) {} }\n', 'step4b: java 草稿仍在');
console.log('  OK step4 切回 cpp 草稿完整恢复');

// Step 5: 切到 Python → 写代码
d.save(866, 'python', 'def main():\n    pass\n');
assert.equal(d.load(866, 'python'), 'def main():\n    pass\n', 'step5: python 草稿写入');
console.log('  OK step5 切到 python 写代码');

// Step 6: 用户点"重置 cpp"
d.clear(866, 'cpp');
assert.equal(d.load(866, 'cpp'),    '',                          'step6a: 重置后 cpp 草稿为空');
assert.equal(d.load(866, 'java'),   'class Main { public static void main(String[] a) {} }\n', 'step6b: java 不受影响');
assert.equal(d.load(866, 'python'), 'def main():\n    pass\n',   'step6c: python 不受影响');
console.log('  OK step6 重置 cpp 不影响 java/python');

// Step 7: 用户点"提交 java" 成功 → clear java
d.clear(866, 'java');
assert.equal(d.load(866, 'java'), '', 'step7a: 提交后 java 草稿清空');
assert.equal(d.load(866, 'python'), 'def main():\n    pass\n', 'step7b: python 仍存在（未提交）');
console.log('  OK step7 提交成功清 java 草稿，其他语言保留');

// Step 8: 切到 c 写入
d.save(866, 'c', '#include <stdio.h>\nint main(){return 0;}\n');
assert.equal(d.load(866, 'c'), '#include <stdio.h>\nint main(){return 0;}\n', 'step8: c 草稿写入');
console.log('  OK step8 c 语言也能写草稿');

// 验收：所有 5 种语言都覆盖了
const allKeys = [...s.getItem ? Object.keys({}) : []];  // 取键
const langs = ['cpp', 'c', 'java', 'python', 'go'];
for (const L of langs) {
    if (L === 'cpp' || L === 'java') continue;  // 这两个已被清
    // 至少能写能读
    d.save(866, L, '// ' + L);
    assert.equal(d.load(866, L), '// ' + L, L + ' 草稿可写可读');
}
console.log('  OK 5 种语言草稿可独立存取');

console.log('---');
console.log('E2E: ALL PASSED');
