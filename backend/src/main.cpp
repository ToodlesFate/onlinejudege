// =============================================================================
//  oj_backend — 主入口
//  依据 SPEC §3.2.2：装配 Common + Http + Domain + Infra 四层
//
//  路由：
//    GET  /api/health
//    POST /api/auth/register
//
//  用法：
//      oj_backend --config <path> [--log-dir <path>]
//      oj_backend --print-config <path>   （调试用，dump 解析后的配置到 stdout）
// =============================================================================

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "common/config.hpp"
#include "common/error_code.hpp"
#include "common/version.hpp"
#include "domain/auth_service.hpp"
#include "domain/judge_dispatcher.hpp"
#include "domain/problem_repository.hpp"
#include "domain/problem_service.hpp"
#include "domain/submission_repository.hpp"
#include "domain/submission_service.hpp"
#include "http/HttpServer.hpp"
#include "http/handlers/admin_problem_handler.hpp"
#include "http/handlers/auth_handler.hpp"
#include "http/handlers/health_handler.hpp"
#include "http/handlers/problem_handler.hpp"
#include "http/handlers/submission_handler.hpp"
#include "http/middleware/middleware.hpp"
#include "infra/docker_client.hpp"
#include "infra/docker_judge_adapter.hpp"
#include "infra/jwt_service.hpp"
#include "infra/mysql_client.hpp"
#include "infra/password_hasher.hpp"
#include "infra/problem_repo.hpp"
#include "infra/submission_repo.hpp"
#include "infra/tag_repo.hpp"
#include "infra/testcase_repo.hpp"
#include "infra/user_repo.hpp"

namespace {

std::atomic<oj::http::HttpServer*>   g_server{nullptr};
std::atomic<oj::domain::JudgeDispatcher*> g_dispatcher{nullptr};

void on_signal(int sig) {
    spdlog::warn("received signal {}, shutting down", sig);
    if (auto* d = g_dispatcher.load(); d != nullptr) {
        d->stop();
    }
    if (auto* s = g_server.load(); s != nullptr) {
        s->stop();
    }
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);
}

spdlog::level::level_enum parse_level(const std::string& s) {
    using namespace spdlog::level;
    if (s == "trace")    return trace;
    if (s == "debug")    return debug;
    if (s == "info")     return info;
    if (s == "warn" || s == "warning") return warn;
    if (s == "error" || s == "err")    return err;
    if (s == "critical") return critical;
    if (s == "off")      return off;
    return info;
}

void init_logger(const oj::common::LogConfig& log) {
    std::vector<spdlog::sink_ptr> sinks;

    if (log.stdout_console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    try {
        std::filesystem::create_directories(log.dir);
        auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            (log.dir / "oj_backend.log").string(),
            100 * 1024 * 1024,  // 100 MB
            10                  // 10 files
        );
        sinks.push_back(std::move(file));
    } catch (const std::exception& e) {
        std::cerr << "[warn] failed to open log file under " << log.dir
                  << ": " << e.what() << " — falling back to stdout only\n";
    }

    auto logger = std::make_shared<spdlog::logger>("oj_backend", sinks.begin(), sinks.end());
    logger->set_level(parse_level(log.level));
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
}

struct Args {
    std::filesystem::path config_path{"config/default.json"};
    std::filesystem::path log_dir_override{};
    bool print_config{false};
};

std::string_view next_or_empty(int argc, char** argv, int& i) {
    if (i + 1 >= argc) {
        return {};
    }
    return argv[++i];
}

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string_view cur = argv[i];
        if (cur == "--config" || cur == "-c") {
            auto v = next_or_empty(argc, argv, i);
            if (v.empty()) throw std::runtime_error("--config requires a path");
            a.config_path = std::string{v};
        } else if (cur == "--log-dir") {
            auto v = next_or_empty(argc, argv, i);
            if (v.empty()) throw std::runtime_error("--log-dir requires a path");
            a.log_dir_override = std::string{v};
        } else if (cur == "--print-config") {
            auto v = next_or_empty(argc, argv, i);
            if (v.empty()) throw std::runtime_error("--print-config requires a path");
            a.config_path = std::string{v};
            a.print_config = true;
        } else if (cur == "--help" || cur == "-h") {
            std::cout << "oj_backend " << OJ_VERSION_STRING
                      << "\n  --config <path>        config json (default: config/default.json)"
                         "\n  --log-dir <path>       override log dir"
                         "\n  --print-config <path>  load config and dump parsed values, then exit"
                         "\n  --help                 show this help\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + std::string{cur});
        }
    }
    return a;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace oj;

    Args args;
    try {
        args = parse_args(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "[fatal] " << e.what() << "\n";
        return 2;
    }

    common::AppConfig cfg;
    try {
        cfg = common::AppConfig::load(args.config_path);
    } catch (const common::ConfigError& e) {
        std::cerr << "[fatal] config load failed: " << e.what() << "\n";
        return 2;
    }

    if (!args.log_dir_override.empty()) {
        cfg.log.dir = args.log_dir_override;
    }

    if (args.print_config) {
        std::cout << "{\"server\":" << "{"
                  << "\"host\":\"" << cfg.server.host << "\","
                  << "\"port\":"  << cfg.server.port  << ","
                  << "\"thread_pool_size\":" << cfg.server.thread_pool_size
                  << "},"
                  << "\"mysql\":" << "{"
                  << "\"host\":\"" << cfg.mysql.host << "\","
                  << "\"port\":"  << cfg.mysql.port  << ","
                  << "\"user\":\"" << cfg.mysql.user << "\","
                  << "\"database\":\"" << cfg.mysql.database << "\","
                  << "\"pool_size\":" << cfg.mysql.pool_size
                  << "},"
                  << "\"jwt\":" << "{"
                  << "\"issuer\":\"" << cfg.jwt.issuer << "\","
                  << "\"access_ttl_sec\":" << cfg.jwt.access_ttl_sec << ","
                  << "\"refresh_ttl_sec\":" << cfg.jwt.refresh_ttl_sec
                  << "},"
                  << "\"log\":"   << "{"
                  << "\"level\":\"" << cfg.log.level << "\","
                  << "\"dir\":\""   << cfg.log.dir.string() << "\","
                  << "\"stdout\":" << (cfg.log.stdout_console ? "true" : "false")
                  << "}}\n";
        return 0;
    }

    init_logger(cfg.log);
    spdlog::info("oj_backend {} starting up; config={}", OJ_VERSION_STRING, args.config_path.string());

    install_signal_handlers();

    // -------------------------------------------------------------------
    //  装配 Infra 层：MysqlClient → MysqlUserRepo
    //                  PasswordHasher + JwtService
    // -------------------------------------------------------------------
    auto mysql = std::make_shared<infra::MysqlClient>(cfg.mysql);
    try {
        mysql->connect();
    } catch (const std::exception& e) {
        spdlog::error("MysqlClient::connect failed: {} — /api/auth/* will return 503",
                      e.what());
        // 仍继续启动，让 /api/health 仍可访问；register handler 检测 ready
        // 状态后返回 503。
    }

    auto users  = std::make_shared<infra::MysqlUserRepo>(mysql);
    auto hasher = std::make_shared<infra::PasswordHasher>();
    auto jwt    = std::make_shared<infra::JwtService>(cfg.jwt);

    auto auth_service = std::make_shared<domain::AuthService>(users, hasher, jwt);

    // -------------------------------------------------------------------
    //  Problem 域装配 —— repo (Infra) + service (Domain) + handler (Http)
    // -------------------------------------------------------------------
    auto problems_repo    = std::make_shared<infra::MysqlProblemRepo>(mysql);
    auto testcases_repo   = std::make_shared<infra::MysqlTestcaseRepo>(mysql);
    auto tags_repo         = std::make_shared<infra::MysqlTagRepo>(mysql);
    auto problem_service  = std::make_shared<domain::ProblemService>(
        problems_repo, testcases_repo, tags_repo);

    // -------------------------------------------------------------------
    //  Judge 子系统 —— SubmissionRepo + DockerClient + JudgeDispatcher
    //   注意：cfg 在下方会被 move 进 HttpServer；这里先把 cfg.judge 拷贝一份
    //   出来，避免 use-after-move。
    // -------------------------------------------------------------------
    auto submissions_repo = std::make_shared<infra::MysqlSubmissionRepo>(mysql);
    std::shared_ptr<oj::domain::JudgeDispatcher> dispatcher;
    if (mysql->is_ready()) {
        auto docker_client    = std::make_shared<infra::DockerClient>(cfg.judge.docker);
        docker_client->set_work_root(cfg.judge.work_root);
        auto docker_adapter   = std::make_shared<infra::DockerJudgeAdapter>(docker_client);
        dispatcher = std::make_shared<oj::domain::JudgeDispatcher>(
            cfg.judge, submissions_repo, docker_adapter);
        dispatcher->start();
        g_dispatcher.store(dispatcher.get(), std::memory_order_release);
    } else {
        spdlog::warn("MySQL not ready; JudgeDispatcher not started (submissions will queue but never be picked up)");
    }

    // -------------------------------------------------------------------
    //  Submission 域装配 —— SubmissionService（HTTP handler 用）
    //   即便 dispatcher 没起，POST/GET 路由也要可访问（DB 不可用走 503）
    // -------------------------------------------------------------------
    auto submission_service = std::make_shared<domain::SubmissionService>(
        submissions_repo, problems_repo, testcases_repo, cfg.judge.code_max_bytes);

    // -------------------------------------------------------------------
    //  Http 层
    // -------------------------------------------------------------------
    http::HttpServer server(std::move(cfg));

    server.get("/api/health", [&server](const httplib::Request& req, httplib::Response& res) {
        http::handlers::health(req, res, server.uptime_ms());
    });

    http::handlers::register_auth_routes(server, auth_service,
                                         [mysql]() { return mysql->is_ready(); });

    http::handlers::register_problem_routes(server, problem_service,
                                           [mysql]() { return mysql->is_ready(); });

    http::handlers::register_admin_problem_routes(server, problem_service, jwt,
                                                  [mysql]() { return mysql->is_ready(); });

    http::handlers::register_submission_routes(server, submission_service, jwt,
                                               [mysql]() { return mysql->is_ready(); });

    g_server.store(&server, std::memory_order_release);

    // ---- Phase 7: 横切中间件 (access log + 安全响应头) ----------------
    // 注意:install_exception_middleware() 已在 server.listen() 内部调用,
    // 这里只补 access log 与安全响应头。
    oj::http::middleware::install_access_log(server);
    oj::http::middleware::install_security_headers(server);

    std::string reason;
    if (!server.listen(&reason)) {
        spdlog::critical("failed to start http server: {}", reason);
        return 1;
    }
    spdlog::info("oj_backend exited cleanly");
    return 0;
}
