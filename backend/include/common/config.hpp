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

// SPEC §3.2.3 / §7.2
// libmysqlclient / MariaDB 兼容 client 通过 unix socket 或 TCP 3306 接入 MySQL 容器。
// host="mysql" 是 docker-compose 网络内的服务名；本机调试可改成 "127.0.0.1" + 3306 端口映射。
struct MysqlConfig {
    std::string  host{"mysql"};
    std::uint16_t port{3306};
    std::string  user{"oj"};
    std::string  password{"oj"};
    std::string  database{"oj"};
    int          pool_size{8};
    int          connect_timeout_sec{5};
};

// SPEC §2.1 / §3.2.2 / §3.2.3
// JwtService 用 HS256 签发/校验 access 与 refresh token；
// secret 必须 ≥ 32 字节随机；部署时由 Secret Manager 注入。
struct JwtConfig {
    std::string  secret{"change-me-in-prod-please-replace-with-32+random-bytes"};
    int          access_ttl_sec{7200};     // 2 小时
    int          refresh_ttl_sec{604800};  // 7 天
    std::string  issuer{"onlinejudge"};
};

struct AppConfig {
    ServerConfig server{};
    LogConfig    log{};
    MysqlConfig  mysql{};
    JwtConfig    jwt{};

    [[nodiscard]] static AppConfig load(const std::filesystem::path& path);
    [[nodiscard]] static AppConfig load_from_string(std::string_view json_text);
};

}  // namespace oj::common
