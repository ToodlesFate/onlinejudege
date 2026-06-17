#pragma once
#include "judge/types.hpp"
#include <optional>
#include <string>

namespace judge {

// 读 meta.json —— 返回 nullopt 表示文件不存在 / 字段缺失 / 字段类型错
//   失败时把 reason 写到 err（若非 nullptr）
std::optional<Limits> read_meta_file(const std::string& path, std::string* err = nullptr);

}  // namespace judge
