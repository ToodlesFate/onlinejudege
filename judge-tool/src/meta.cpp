#include "meta.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

namespace judge {

using nlohmann::json;

static Language parse_language(const std::string& s) {
    if (s == "c")      return Language::C;
    if (s == "cpp")    return Language::Cpp;
    if (s == "java")   return Language::Java;
    if (s == "python") return Language::Python;
    if (s == "go")     return Language::Go;
    return Language::Unknown;
}

std::optional<Limits> read_meta_file(const std::string& path, std::string* err) {
    std::ifstream f(path);
    if (!f) {
        if (err) *err = "cannot open meta file: " + path;
        return std::nullopt;
    }
    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        if (err) *err = std::string("meta parse error: ") + e.what();
        return std::nullopt;
    }

    Limits L;
    try {
        if (!j.contains("language") || !j["language"].is_string()) {
            if (err) *err = "meta.language missing or not string";
            return std::nullopt;
        }
        L.language = parse_language(j["language"].get<std::string>());
        if (L.language == Language::Unknown) {
            if (err) *err = "meta.language unsupported: " + j["language"].get<std::string>();
            return std::nullopt;
        }

        if (j.contains("time_limit_ms"))   L.time_ms = j["time_limit_ms"].get<int>();
        if (j.contains("memory_limit_mb")) L.mem_mb  = j["memory_limit_mb"].get<int>();
        if (j.contains("output_limit_mb")) L.out_mb  = j["output_limit_mb"].get<int>();
    } catch (const std::exception& e) {
        if (err) *err = std::string("meta field error: ") + e.what();
        return std::nullopt;
    }

    // SPEC §2.2.1 范围校验（防错配 / 越界）
    if (L.time_ms < 1 || L.time_ms > 10'000) {
        if (err) *err = "time_limit_ms out of range (1..10000): " + std::to_string(L.time_ms);
        return std::nullopt;
    }
    if (L.mem_mb < 16 || L.mem_mb > 4096) {
        if (err) *err = "memory_limit_mb out of range (16..4096): " + std::to_string(L.mem_mb);
        return std::nullopt;
    }
    if (L.out_mb < 1 || L.out_mb > 256) {
        if (err) *err = "output_limit_mb out of range (1..256): " + std::to_string(L.out_mb);
        return std::nullopt;
    }
    return L;
}

}  // namespace judge
