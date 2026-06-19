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

// SPEC §3.2.3 / §2.6 可观测
//   - spdlog 文件日志（轮转 100MB × 10 份） + stdout 容器日志
//   - 实际尺寸 = max_size_mb * 1024 * 1024 字节；max_files 含当前活跃文件
struct LogConfig {
    std::string level{"info"};
    std::filesystem::path dir{"/var/log/oj"};
    bool stdout_console{true};
    // 单个日志文件最大尺寸（MB），默认 100。SPEC §3.2.3 固定 100。
    int max_size_mb{100};
    // 保留的轮转文件数（含活跃文件），默认 10。SPEC §3.2.3 固定 10。
    int max_files{10};
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

struct DockerConfig {
    // unix:///var/run/docker.sock （典型） 或 tcp://host:2375
    std::string  host{"unix:///var/run/docker.sock"};
    // Docker Engine API version（SPEC §6.1 固定 v1.41 +）
    std::string  api_version{"v1.41"};
    // 单次 HTTP 请求超时（秒）
    int          request_timeout_sec{120};
    // 容器 wait 超时 = time_limit + 此 buffer（SPEC §6.1：30s）
    int          container_wait_buffer_sec{30};
};

struct JudgeImageConfig {
    std::string c     {"judge-c:1.0"};
    std::string cpp   {"judge-cpp:1.0"};
    std::string java  {"judge-java:1.0"};
    std::string python{"judge-python:1.0"};
    std::string go    {"judge-go:1.0"};
};

struct JudgeConfig {
    int                worker_count{4};
    int                poll_interval_ms{500};
    int                default_time_limit_ms{2000};
    int                default_memory_limit_mb{256};
    int                default_output_limit_mb{64};
    int                code_max_bytes{65536};
    int                problem_md_max_bytes{65536};
    std::filesystem::path work_root{"/tmp/oj"};

    DockerConfig        docker{};
    JudgeImageConfig    images{};
};

struct AppConfig {
    ServerConfig server{};
    LogConfig    log{};
    MysqlConfig  mysql{};
    JwtConfig    jwt{};
    JudgeConfig  judge{};

    [[nodiscard]] static AppConfig load(const std::filesystem::path& path);
    [[nodiscard]] static AppConfig load_from_string(std::string_view json_text);
};

}  // namespace oj::common
