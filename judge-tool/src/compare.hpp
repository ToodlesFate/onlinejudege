#pragma once
#include <string>

namespace judge {

// "diff -b" 风格输出比对：
//   - 按行比较
//   - 忽略每行末尾的空白字符（空格、tab）
//   - 不忽略行内的空白差异
//   - 完全相同 → true
//
// 如果 first_diff 非空，把首条差异行写入（"expected | actual" 形式）
bool compare_outputs(const std::string& expected,
                     const std::string& actual,
                     std::string* first_diff = nullptr);

}  // namespace judge
