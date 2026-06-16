#include "common/config.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace oj::common {

namespace {

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw ConfigError("cannot open config file: " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string slurp(std::string_view text) {
    return std::string{text};
}

template <typename Src>
AppConfig parse(Src&& source, std::string_view origin_for_error) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(slurp(std::forward<Src>(source)));
    } catch (const nlohmann::json::parse_error& e) {
        throw ConfigError(std::string{"invalid json in "} + std::string{origin_for_error} + ": " + e.what());
    }

    AppConfig cfg;

    if (auto it = root.find("server"); it != root.end() && it->is_object()) {
        const auto& s = *it;
        if (auto p = s.find("host"); p != s.end() && p->is_string()) cfg.server.host = p->get<std::string>();
        if (auto p = s.find("port"); p != s.end() && p->is_number_integer()) {
            cfg.server.port = static_cast<std::uint16_t>(p->get<int>());
        }
        if (auto p = s.find("thread_pool_size"); p != s.end() && p->is_number_integer()) {
            cfg.server.thread_pool_size = p->get<int>();
        }
    }

    if (auto it = root.find("log"); it != root.end() && it->is_object()) {
        const auto& l = *it;
        if (auto p = l.find("level"); p != l.end() && p->is_string()) cfg.log.level = p->get<std::string>();
        if (auto p = l.find("dir");   p != l.end() && p->is_string()) cfg.log.dir   = p->get<std::string>();
        if (auto p = l.find("stdout"); p != l.end() && p->is_boolean()) cfg.log.stdout_console = p->get<bool>();
    }

    if (auto it = root.find("mysql"); it != root.end() && it->is_object()) {
        const auto& m = *it;
        if (auto p = m.find("host");     p != m.end() && p->is_string())     cfg.mysql.host     = p->get<std::string>();
        if (auto p = m.find("port");     p != m.end() && p->is_number_integer())
            cfg.mysql.port = static_cast<std::uint16_t>(p->get<int>());
        if (auto p = m.find("user");     p != m.end() && p->is_string())     cfg.mysql.user     = p->get<std::string>();
        if (auto p = m.find("password"); p != m.end() && p->is_string())     cfg.mysql.password = p->get<std::string>();
        if (auto p = m.find("database"); p != m.end() && p->is_string())     cfg.mysql.database = p->get<std::string>();
        if (auto p = m.find("pool_size"); p != m.end() && p->is_number_integer()) cfg.mysql.pool_size = p->get<int>();
        if (auto p = m.find("connect_timeout_sec"); p != m.end() && p->is_number_integer())
            cfg.mysql.connect_timeout_sec = p->get<int>();
    }

    if (auto it = root.find("jwt"); it != root.end() && it->is_object()) {
        const auto& j = *it;
        if (auto p = j.find("secret");          p != j.end() && p->is_string()) cfg.jwt.secret          = p->get<std::string>();
        if (auto p = j.find("access_ttl_sec");  p != j.end() && p->is_number_integer()) cfg.jwt.access_ttl_sec  = p->get<int>();
        if (auto p = j.find("refresh_ttl_sec"); p != j.end() && p->is_number_integer()) cfg.jwt.refresh_ttl_sec = p->get<int>();
        if (auto p = j.find("issuer");          p != j.end() && p->is_string()) cfg.jwt.issuer          = p->get<std::string>();
    }

    return cfg;
}

}  // namespace

AppConfig AppConfig::load(const std::filesystem::path& path) {
    return parse(path, path.string());
}

AppConfig AppConfig::load_from_string(std::string_view json_text) {
    return parse(json_text, "<inline>");
}

}  // namespace oj::common
