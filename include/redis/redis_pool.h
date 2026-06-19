#ifndef LINUX_SERVER_INCLUDE_REDIS_REDIS_POOL_H_
#define LINUX_SERVER_INCLUDE_REDIS_REDIS_POOL_H_

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>

#include "redis/redis_connection.h"

namespace chat {

class RedisPool;

class PooledRedisConnection {
   public:
    PooledRedisConnection(RedisPool *pool, std::unique_ptr<RedisConnection> connection);
    ~PooledRedisConnection();

    PooledRedisConnection(const PooledRedisConnection &) = delete;
    PooledRedisConnection &operator=(const PooledRedisConnection &) = delete;
    PooledRedisConnection(PooledRedisConnection &&other) noexcept;
    PooledRedisConnection &operator=(PooledRedisConnection &&other) noexcept;

    RedisConnection *operator->() noexcept { return connection_.get(); }
    explicit operator bool() const noexcept { return connection_ != nullptr; }
    void MarkBad() noexcept { reusable_ = false; }

   private:
    RedisPool *pool_ = nullptr;
    std::unique_ptr<RedisConnection> connection_;
    bool reusable_ = true;
};

struct BorrowRedisConnectionResult {
    std::optional<PooledRedisConnection> connection;
    RedisError error = RedisError::kNone;
    std::string message;

    bool ok() const noexcept { return connection.has_value(); }
};

struct RedisPoolInitResult {
    bool success = false;
    RedisError error = RedisError::kNone;
    std::string message;

    bool ok() const noexcept { return success; }
};

// 连接池只在借还边界共享连接，借出的 hiredis context 始终由单线程独占。
class RedisPool {
   public:
    struct Stats {
        std::size_t idle = 0;
        std::size_t total = 0;
        std::size_t waiting = 0;
        std::size_t invalidated = 0;
        std::size_t timeouts = 0;
    };

    explicit RedisPool(const RedisConfig &config);
    RedisPool(const RedisConfig &config, std::shared_ptr<IRedisConnectionFactory> factory);
    ~RedisPool();

    RedisPoolInitResult Init();
    BorrowRedisConnectionResult Borrow();
    Stats GetStats();
    void Stop();

   private:
    friend class PooledRedisConnection;

    void Return(std::unique_ptr<RedisConnection> connection, bool reusable);
    std::unique_ptr<RedisConnection> CreateConnected(RedisCommandResult *error);

    RedisConfig config_;
    std::shared_ptr<IRedisConnectionFactory> factory_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::queue<std::unique_ptr<RedisConnection>> idle_;
    std::size_t total_ = 0;
    bool initialized_ = false;
    bool stopping_ = false;
    Stats stats_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_REDIS_REDIS_POOL_H_
