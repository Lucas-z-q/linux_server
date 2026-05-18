#ifndef LINUX_SERVER_INCLUDE_DB_DB_POOL_H_
#define LINUX_SERVER_INCLUDE_DB_DB_POOL_H_

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>

#include "config/db_config.h"
#include "db/db_connection.h"
#include "db/db_pool_config.h"

// 本文件声明数据库连接池抽象。
// 当前阶段可以先提供轻量封装，后续再补充连接复用与线程安全实现。
//
// TODO(lzq): 明确连接池使用的底层 MySQL C++ 客户端库。
// TODO(lzq): 为连接借出、归还和销毁补充完整接口。
// TODO(lzq): 增加连接健康检查和自动重连策略。

namespace chat {

class DbPool;

class PooledConnection {
   public:
    PooledConnection() = default;
    PooledConnection(DbPool* pool, std::unique_ptr<DbConnection> conn,
                     std::chrono::steady_clock::time_point create_time);

    ~PooledConnection();

    PooledConnection(const PooledConnection&) = delete;
    PooledConnection& operator=(const PooledConnection&) = delete;

    PooledConnection(PooledConnection&& other) noexcept;
    PooledConnection& operator=(PooledConnection&& other) noexcept;

    DbConnection* operator->() noexcept;
    DbConnection& operator*() noexcept;
    explicit operator bool() const noexcept;
    // 标记该连接已经损坏，归还时直接销毁而不放回空闲队列
    void markBad() noexcept;

   private:
    DbPool* pool_ = nullptr;
    std::unique_ptr<DbConnection> conn_;
    std::chrono::steady_clock::time_point create_time_;
    bool reusable_ = true;
};

// 负责初始化并管理数据库连接资源。
class DbPool {
   public:
    explicit DbPool(const DbConfig& config);
    DbPool(const DbConfig& config, const DbPoolConfig& pool_config);
    ~DbPool();

    DbPool(const DbPool&) = delete;
    DbPool& operator=(const DbPool&) = delete;

    bool init();
    std::optional<PooledConnection> borrow();
    void stop();

    // 获取当前连接池统计信息
    struct Stats {
        std::size_t current_idle = 0;       // 当前空闲数
        std::size_t current_total = 0;      // 当前总连接数
        std::size_t waiting_threads = 0;    // 等待线程数
        std::size_t total_created = 0;      // 累计成功创建数
        std::size_t total_ping_failed = 0;  // 累计 ping 失败次数 (借出前检查)
        std::size_t total_invalidated = 0;  // 累计连接失效次数 (归还时或业务标记)
        std::size_t total_expired = 0;      // 累计连接过期次数 (空闲或生命周期)
        std::size_t total_timeouts = 0;     // 累计超时数
    };
    Stats getStats();

    friend class PooledConnection;

   private:
    // 连接元信息包装，用于支持 max_lifetime_ms 和 idle_timeout_ms
    struct IdleConnection {
        std::unique_ptr<DbConnection> conn;
        std::chrono::steady_clock::time_point create_time;
        std::chrono::steady_clock::time_point last_return_time;
    };
    void returnConnection(std::unique_ptr<DbConnection> conn, std::chrono::steady_clock::time_point create_time,
                          bool reusable);
    std::unique_ptr<DbConnection> createConnection();

    bool isExpired(const IdleConnection& idle, std::chrono::steady_clock::time_point now);

    DbConfig config_;
    DbPoolConfig pool_config_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<IdleConnection> idle_connections_;
    std::size_t total_connections_ = 0;
    bool stopping_ = false;
    bool initialized_ = false;

    Stats stats_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_POOL_H_
