#include "infra/mysql_client.hpp"

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>

namespace oj::infra {

// ---------------------------------------------------------------------------
//  Lease
// ---------------------------------------------------------------------------
MysqlClient::Lease::Lease(MysqlClient* owner, MYSQL* conn)
    : owner_(owner), conn_(conn) {}

MysqlClient::Lease::Lease(Lease&& other) noexcept
    : owner_(other.owner_), conn_(other.conn_) {
    other.owner_ = nullptr;
    other.conn_  = nullptr;
}

MysqlClient::Lease& MysqlClient::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = other.owner_;
        conn_  = other.conn_;
        other.owner_ = nullptr;
        other.conn_  = nullptr;
    }
    return *this;
}

MysqlClient::Lease::~Lease() {
    release();
}

void MysqlClient::Lease::release() noexcept {
    if (owner_ != nullptr && conn_ != nullptr) {
        owner_->give_back(conn_);
        owner_ = nullptr;
        conn_  = nullptr;
    }
}

// ---------------------------------------------------------------------------
//  MysqlClient
// ---------------------------------------------------------------------------
MysqlClient::MysqlClient(common::MysqlConfig cfg) : config_(std::move(cfg)) {
    // 仅初始化 libmysqlclient 客户端库；连接在 connect() 中建立
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        throw MysqlError("mysql_library_init failed");
    }
}

MysqlClient::~MysqlClient() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        shutting_down_ = true;
    }
    cv_.notify_all();
    for (MYSQL* c : all_) {
        if (c != nullptr) {
            mysql_close(c);
        }
    }
    all_.clear();
    idle_.clear();
    mysql_library_end();
}

MYSQL* MysqlClient::open_one() {
    MYSQL* m = mysql_init(nullptr);
    if (m == nullptr) {
        throw MysqlError("mysql_init returned nullptr");
    }

    // 连接超时（秒）—— 防止网络不可达时 hang 住整个 backend 启动
    unsigned int timeout = static_cast<unsigned int>(
        config_.connect_timeout_sec > 0 ? config_.connect_timeout_sec : 5);
    mysql_options(m, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
    mysql_options(m, MYSQL_OPT_READ_TIMEOUT,    &timeout);
    mysql_options(m, MYSQL_OPT_WRITE_TIMEOUT,   &timeout);

    bool reconnect = true;
    mysql_options(m, MYSQL_OPT_RECONNECT, &reconnect);

    // utf8mb4 —— SPEC §4.3 字符集要求
    mysql_options(m, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    if (mysql_real_connect(
            m,
            config_.host.c_str(),
            config_.user.c_str(),
            config_.password.c_str(),
            config_.database.c_str(),
            config_.port,
            nullptr,            // unix socket
            CLIENT_MULTI_STATEMENTS) == nullptr) {
        std::string err = mysql_error(m);
        unsigned int errno_ = mysql_errno(m);
        mysql_close(m);
        throw MysqlError("mysql_real_connect(" + config_.host + ":" +
                         std::to_string(config_.port) + "/" +
                         config_.database + ") failed: [" +
                         std::to_string(errno_) + "] " + err);
    }
    return m;
}

void MysqlClient::connect() {
    if (ready_) {
        return;
    }
    const int n = config_.pool_size > 0 ? config_.pool_size : 1;
    spdlog::info("MysqlClient: connecting to {}:{} as {} (pool_size={})",
                 config_.host, config_.port, config_.user, n);

    std::vector<MYSQL*> opened;
    try {
        for (int i = 0; i < n; ++i) {
            opened.push_back(open_one());
        }
    } catch (...) {
        for (MYSQL* c : opened) {
            mysql_close(c);
        }
        throw;
    }

    {
        std::lock_guard<std::mutex> lk(mu_);
        all_  = std::move(opened);
        idle_.clear();
        for (MYSQL* c : all_) idle_.push_back(c);
        ready_ = true;
    }
    spdlog::info("MysqlClient: pool ready ({} connections)", all_.size());
}

std::size_t MysqlClient::available() const {
    std::lock_guard<std::mutex> lk(mu_);
    return idle_.size();
}

std::string MysqlClient::escape(std::string_view s) {
    // 防御：空串 / 极短 buffer 也安全（mysql_real_escape_string 接受 0 长度）
    std::string out(s.size() * 2 + 1, '\0');
    // 需要一个非 const MYSQL* 给 mysql_real_escape_string_quote；
    // 由于 escape() 语义是 const（"基于当前连接的字符集"），我们从一个
    // 空闲连接上借出 → 用完立刻归还，开销可忽略。
    // 这里走简化路径：调用者传入 string_view，输出转义后的字符串。
    // libmysqlclient 21 在无活跃连接时 mysql_real_escape_string_quote 仍
    // 需要一个连接，但用 mysql_hex_quote 不需要；为安全起见，我们走
    // mysql_real_escape_string 路径。
    std::unique_lock<std::mutex> lk(mu_);
    if (idle_.empty()) {
        // 没有空闲连接——退而求其次用 mysql_real_escape_string 的无连接版本
        // 实际是 mysql_cset_escape，但接口不公开。改用"两倍长度+手工转义"
        // 的保守做法，仅在池耗尽时使用。
        out.clear();
        out.reserve(s.size() * 2);
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '\'': out += "\\'";  break;
                case '"':  out += "\\\""; break;
                case '\0': out += "\\0";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\b': out += "\\b";  break;
                case '\t': out += "\\t";  break;
                case '\x1A': out += "\\Z"; break;
                default:   out += c;       break;
            }
        }
        return out;
    }
    MYSQL* c = idle_.front();
    idle_.pop_front();
    lk.unlock();

    unsigned long n = mysql_real_escape_string(c, out.data(), s.data(), s.size());
    out.resize(n);

    // 立刻归还
    lk.lock();
    idle_.push_back(c);
    return out;
}

MysqlClient::Lease MysqlClient::acquire() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [&] { return shutting_down_ || !idle_.empty(); });
    if (shutting_down_) {
        throw MysqlError("MysqlClient is shutting down");
    }
    MYSQL* c = idle_.front();
    idle_.pop_front();
    return Lease{this, c};
}

void MysqlClient::give_back(MYSQL* conn) noexcept {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shutting_down_) {
            // 关闭流程中：直接 close，不放回池
            mysql_close(conn);
            return;
        }
        idle_.push_back(conn);
    }
    cv_.notify_one();
}

}  // namespace oj::infra
