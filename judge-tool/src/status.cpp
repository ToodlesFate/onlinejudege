#include "judge/types.hpp"

namespace judge {

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::AC:  return "AC";
        case Status::WA:  return "WA";
        case Status::TLE: return "TLE";
        case Status::MLE: return "MLE";
        case Status::OLE: return "OLE";
        case Status::RE:  return "RE";
        case Status::CE:  return "CE";
        case Status::SE:  return "SE";
    }
    return "SE";
}

Status status_from_string(const std::string& s) noexcept {
    if (s == "AC")  return Status::AC;
    if (s == "WA")  return Status::WA;
    if (s == "TLE") return Status::TLE;
    if (s == "MLE") return Status::MLE;
    if (s == "OLE") return Status::OLE;
    if (s == "RE")  return Status::RE;
    if (s == "CE")  return Status::CE;
    if (s == "SE")  return Status::SE;
    return Status::SE;
}

const char* language_id(Language l) noexcept {
    switch (l) {
        case Language::C:      return "c";
        case Language::Cpp:    return "cpp";
        case Language::Java:   return "java";
        case Language::Python: return "python";
        case Language::Go:     return "go";
        default:               return "unknown";
    }
}

int severity(Status s) noexcept {
    // 越大越坏 —— 用于 overall 聚合时取 max
    // 顺序：AC < WA < RE < OLE < TLE < MLE < CE < SE
    // 解释：AC 最好；WA 输出错；RE 崩了；OLE 输出超量；
    //       TLE 跑得慢；MLE 吃内存；CE 编译没过；SE 系统错（最坏）
    switch (s) {
        case Status::AC:  return 0;
        case Status::WA:  return 1;
        case Status::RE:  return 2;
        case Status::OLE: return 3;
        case Status::TLE: return 4;
        case Status::MLE: return 5;
        case Status::CE:  return 6;
        case Status::SE:  return 7;
    }
    return 7;
}

}  // namespace judge
