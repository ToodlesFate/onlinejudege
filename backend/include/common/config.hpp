#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace oj::common {

// 配置加载异常 —— main.cpp 捕获后退出
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 强类型包装，避免裸 int 满天飞
struct ServerConfig {
    std::string  host{"0.0.0.0"};
    std::uint16_t port{8080};
    int          thread_pool_size{8};
};

struct LogConfig {
    std::string level{"info"};
    std::filesystem::path dir{"/var/log/oj"};
    bool stdout_console{true};
};

struct AppConfig {
    ServerConfig server{};
    LogConfig    log{};

    [[nodiscard]] static AppConfig load(const std::filesystem::path& path);
    [[nodiscard]] static AppConfig load_from_string(std::string_view json_text);
};

}  // namespace oj::common