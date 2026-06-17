// =============================================================================
//  problem_types.cpp —— 枚举 / 数据形状辅助函数的实现
//  SPEC §4.2 字段序列化（DB ENUM ↔ C++ enum）
// =============================================================================

#include "domain/problem_types.hpp"

#include <string>
#include <string_view>

namespace oj::domain {

// ---------------------------------------------------------------------------
//  Difficulty
// ---------------------------------------------------------------------------
std::string_view to_string(Difficulty d) noexcept {
    switch (d) {
        case Difficulty::Easy:   return "easy";
        case Difficulty::Medium: return "medium";
        case Difficulty::Hard:   return "hard";
    }
    return "easy";  // 兜底：enum 越界 → 走最简单的值
}

std::optional<Difficulty>
difficulty_from_string(std::string_view s) noexcept {
    if (s == "easy")   return Difficulty::Easy;
    if (s == "medium") return Difficulty::Medium;
    if (s == "hard")   return Difficulty::Hard;
    return std::nullopt;
}

// ---------------------------------------------------------------------------
//  Language
// ---------------------------------------------------------------------------
std::string_view to_string(Language l) noexcept {
    switch (l) {
        case Language::C:      return "c";
        case Language::Cpp:    return "cpp";
        case Language::Java:   return "java";
        case Language::Python: return "python";
        case Language::Go:     return "go";
    }
    return "cpp";
}

std::optional<Language>
language_from_string(std::string_view s) noexcept {
    if (s == "c")      return Language::C;
    if (s == "cpp")    return Language::Cpp;
    if (s == "java")   return Language::Java;
    if (s == "python") return Language::Python;
    if (s == "go")     return Language::Go;
    return std::nullopt;
}

}  // namespace oj::domain
