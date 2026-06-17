#include "compare.hpp"
#include <algorithm>
#include <cctype>
#include <vector>

namespace judge {

namespace {

std::string rtrim(const std::string& s) {
    auto end = s.end();
    while (end != s.begin()) {
        unsigned char c = *(end - 1);
        if (c == ' ' || c == '\t' || c == '\r') --end;
        else break;
    }
    return std::string(s.begin(), end);
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(rtrim(cur));
            cur.clear();
        } else if (c != '\r') {
            cur.push_back(c);
        }
    }
    if (!cur.empty() || s.empty() == false) {
        out.push_back(rtrim(cur));
    }
    return out;
}

}  // namespace

bool compare_outputs(const std::string& expected,
                     const std::string& actual,
                     std::string* first_diff) {
    auto e = split_lines(expected);
    auto a = split_lines(actual);

    // SPEC 的"diff -b"：忽略每行末尾的空白
    // 若两个文件总行数不同 → WA
    if (e.size() != a.size()) {
        if (first_diff) {
            *first_diff = "line count differs: expected=" +
                          std::to_string(e.size()) +
                          " actual=" + std::to_string(a.size());
        }
        return false;
    }

    for (size_t i = 0; i < e.size(); ++i) {
        if (e[i] != a[i]) {
            if (first_diff) {
                // 截断长行，避免日志爆
                auto trunc = [](const std::string& s) {
                    return s.size() > 80 ? s.substr(0, 77) + "..." : s;
                };
                *first_diff = "line " + std::to_string(i + 1) +
                              " expected=\"" + trunc(e[i]) +
                              "\" actual=\"" + trunc(a[i]) + "\"";
            }
            return false;
        }
    }
    return true;
}

}  // namespace judge
