#pragma once

// =============================================================================
//  oj::infra::MysqlClient — libmysqlclient 连接池
//  SPEC §3.2.2 "MysqlClient"：封装 libmysqlclient C API，连接池（≥ 8 连接）
//
//  设计要点：
//    1. 启动期调用 connect() 建立 pool_size 个 MYSQL*；连接失败抛 MysqlError
//       让 main.cpp 决定是否降级启动
//    2. acquire() 阻塞借出一个连接；Lease RAII 归还
//    3. 连接不可用时自动 mysql_close + 重建（不向外暴露 MYSQL* 指针的
//       所有权，调用方只通过 Lease 拿到 raw handle）
//    4. 线程安全：mutex 保护 idle queue
//    5. escape() 提供 mysql_real_escape_string 风格的安全转义辅助
// =============================================================================

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mysql.h>

#include "common/config.hpp"

namespace oj::infra {

// MysqlClient / Lease 抛出的所有异常统一基类
class MysqlError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// MysqlClient —— 启动期建立 pool，调用方通过 acquire() 借/还连接
class MysqlClient {
public:
    explicit MysqlClient(common::MysqlConfig cfg);
    ~MysqlClient();

    MysqlClient(const MysqlClient&)            = delete;
    MysqlClient& operator=(const MysqlClient&) = delete;

    // 启动期同步建立 pool_size 个连接，全部失败抛 MysqlError；
    // 部分失败则关闭已建立的，抛 MysqlError。
    void connect();

    // 是否所有池连接都还在 idle 队列（用于 /api/health 等只读探针）
    [[nodiscard]] std::size_t available() const;

    // pool_size + 已借出的连接总数（用于监控）
    [[nodiscard]] std::size_t size() const noexcept { return config_.pool_size; }

    // 池是否已经成功初始化过
    [[nodiscard]] bool is_ready() const noexcept { return ready_; }

    // 转义 string_view，调用方负责把返回值嵌入 SQL 字面量两侧再加单引号
    [[nodiscard]] std::string escape(std::string_view s);

    // -----------------------------------------------------------------
    //  Lease —— RAII 句柄，析构时自动归还连接到池
    // -----------------------------------------------------------------
    class Lease {
    public:
        Lease(MysqlClient* owner, MYSQL* conn);
        ~Lease();

        Lease(const Lease&)            = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        [[nodiscard]] MYSQL* raw() const noexcept { return conn_; }

        // 是否仍持有连接
        [[nodiscard]] explicit operator bool() const noexcept { return conn_ != nullptr; }

        // 主动归还（析构前调用可提前释放；否则会等到 Lease 析构）
        void release() noexcept;

    private:
        MysqlClient* owner_;
        MYSQL*       conn_;
    };

    // 借出一个连接；若池空则阻塞等待。
    // 抛 MysqlError 表示池无法提供（已 shutdown / 初始化失败）。
    [[nodiscard]] Lease acquire();

    // Lease 析构时调用；外部不要直接调用。
    void give_back(MYSQL* conn) noexcept;

private:
    MYSQL* open_one();  // 打开一个新连接；失败抛 MysqlError

    common::MysqlConfig        config_;
    std::vector<MYSQL*>        all_;       // 池中所有连接（用于析构统一 close）
    std::deque<MYSQL*>         idle_;      // 当前空闲连接
    mutable std::mutex         mu_;
    std::condition_variable    cv_;
    bool                       ready_{false};
    bool                       shutting_down_{false};
};

}  // namespace oj::infra
