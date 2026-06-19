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
        if (auto p = l.find("max_size_mb"); p != l.end() && p->is_number_integer()) {
            int v = p->get<int>();
            if (v > 0) {
                cfg.log.max_size_mb = v;
            } else {
                throw ConfigError("log.max_size_mb must be > 0 (got " + std::to_string(v) + ")");
            }
        }
        if (auto p = l.find("max_files"); p != l.end() && p->is_number_integer()) {
            int v = p->get<int>();
            if (v >= 1) {
                cfg.log.max_files = v;
            } else {
                throw ConfigError("log.max_files must be >= 1 (got " + std::to_string(v) + ")");
            }
        }
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

    if (auto it = root.find("judge"); it != root.end() && it->is_object()) {
        const auto& j = *it;
        if (auto p = j.find("worker_count");             p != j.end() && p->is_number_integer()) cfg.judge.worker_count            = p->get<int>();
        if (auto p = j.find("poll_interval_ms");          p != j.end() && p->is_number_integer()) cfg.judge.poll_interval_ms         = p->get<int>();
        if (auto p = j.find("default_time_limit_ms");    p != j.end() && p->is_number_integer()) cfg.judge.default_time_limit_ms   = p->get<int>();
        if (auto p = j.find("default_memory_limit_mb");  p != j.end() && p->is_number_integer()) cfg.judge.default_memory_limit_mb = p->get<int>();
        if (auto p = j.find("default_output_limit_mb");  p != j.end() && p->is_number_integer()) cfg.judge.default_output_limit_mb = p->get<int>();
        if (auto p = j.find("code_max_bytes");           p != j.end() && p->is_number_integer()) cfg.judge.code_max_bytes          = p->get<int>();
        if (auto p = j.find("problem_md_max_bytes");     p != j.end() && p->is_number_integer()) cfg.judge.problem_md_max_bytes    = p->get<int>();
        if (auto p = j.find("work_root");                p != j.end() && p->is_string())           cfg.judge.work_root               = p->get<std::string>();

        if (auto dit = j.find("docker"); dit != j.end() && dit->is_object()) {
            const auto& d = *dit;
            if (auto p = d.find("host");                       p != d.end() && p->is_string()) cfg.judge.docker.host = p->get<std::string>();
            if (auto p = d.find("api_version");                p != d.end() && p->is_string()) cfg.judge.docker.api_version = p->get<std::string>();
            if (auto p = d.find("request_timeout_sec");        p != d.end() && p->is_number_integer()) cfg.judge.docker.request_timeout_sec = p->get<int>();
            if (auto p = d.find("container_wait_buffer_sec"); p != d.end() && p->is_number_integer()) cfg.judge.docker.container_wait_buffer_sec = p->get<int>();
        }

        if (auto iit = j.find("images"); iit != j.end() && iit->is_object()) {
            const auto& ii = *iit;
            if (auto p = ii.find("c");      p != ii.end() && p->is_string()) cfg.judge.images.c      = p->get<std::string>();
            if (auto p = ii.find("cpp");    p != ii.end() && p->is_string()) cfg.judge.images.cpp    = p->get<std::string>();
            if (auto p = ii.find("java");   p != ii.end() && p->is_string()) cfg.judge.images.java   = p->get<std::string>();
            if (auto p = ii.find("python"); p != ii.end() && p->is_string()) cfg.judge.images.python = p->get<std::string>();
            if (auto p = ii.find("go");     p != ii.end() && p->is_string()) cfg.judge.images.go     = p->get<std::string>();
        }
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
