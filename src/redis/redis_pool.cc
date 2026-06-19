#include "redis/redis_pool.h"

#include <chrono>
#include <utility>

namespace chat {

PooledRedisConnection::PooledRedisConnection(RedisPool *pool, std::unique_ptr<RedisConnection> connection)
    : pool_(pool), connection_(std::move(connection)) {}

PooledRedisConnection::~PooledRedisConnection() {
    if (pool_ != nullptr) {
        pool_->Return(std::move(connection_), reusable_);
    }
}

PooledRedisConnection::PooledRedisConnection(PooledRedisConnection &&other) noexcept
    : pool_(other.pool_), connection_(std::move(other.connection_)), reusable_(other.reusable_) {
    other.pool_ = nullptr;
}

PooledRedisConnection &PooledRedisConnection::operator=(PooledRedisConnection &&other) noexcept {
    if (this != &other) {
        if (pool_ != nullptr) {
            pool_->Return(std::move(connection_), reusable_);
        }
        pool_ = other.pool_;
        connection_ = std::move(other.connection_);
        reusable_ = other.reusable_;
        other.pool_ = nullptr;
    }
    return *this;
}

RedisPool::RedisPool(const RedisConfig &config)
    : RedisPool(config, std::make_shared<DefaultRedisConnectionFactory>()) {}

RedisPool::RedisPool(const RedisConfig &config, std::shared_ptr<IRedisConnectionFactory> factory)
    : config_(config), factory_(std::move(factory)) {}

RedisPool::~RedisPool() { Stop(); }

RedisPoolInitResult RedisPool::Init() {
    std::queue<std::unique_ptr<RedisConnection>> created;
    for (std::size_t i = 0; i < config_.pool_size; ++i) {
        RedisCommandResult error;
        std::unique_ptr<RedisConnection> connection = CreateConnected(&error);
        if (!connection) {
            return {.success = false, .error = error.error, .message = error.message};
        }
        created.push(std::move(connection));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    while (!created.empty()) {
        idle_.push(std::move(created.front()));
        created.pop();
        ++total_;
    }
    initialized_ = true;
    stopping_ = false;
    return {.success = true, .error = RedisError::kNone, .message = "init success"};
}

BorrowRedisConnectionResult RedisPool::Borrow() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        if (!initialized_ || stopping_) {
            return {.error = RedisError::kConnectionUnavailable, .message = "redis pool is unavailable"};
        }
        if (!idle_.empty()) {
            std::unique_ptr<RedisConnection> connection = std::move(idle_.front());
            idle_.pop();
            BorrowRedisConnectionResult result;
            result.connection.emplace(this, std::move(connection));
            return result;
        }
        if (total_ < config_.pool_size) {
            ++total_;
            lock.unlock();
            RedisCommandResult error;
            std::unique_ptr<RedisConnection> connection = CreateConnected(&error);
            lock.lock();
            if (!connection) {
                --total_;
                condition_.notify_one();
                return {.error = error.error, .message = error.message};
            }
            BorrowRedisConnectionResult result;
            result.connection.emplace(this, std::move(connection));
            return result;
        }

        ++stats_.waiting;
        const bool ready = condition_.wait_for(lock, std::chrono::milliseconds(config_.command_timeout_ms), [this]() {
            return stopping_ || !idle_.empty() || total_ < config_.pool_size;
        });
        --stats_.waiting;
        if (!ready) {
            ++stats_.timeouts;
            return {.error = RedisError::kTimeout, .message = "borrow redis connection timeout"};
        }
    }
}

RedisPool::Stats RedisPool::GetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result = stats_;
    result.idle = idle_.size();
    result.total = total_;
    return result;
}

void RedisPool::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    initialized_ = false;
    while (!idle_.empty()) {
        idle_.pop();
    }
    total_ = 0;
    condition_.notify_all();
}

void RedisPool::Return(std::unique_ptr<RedisConnection> connection, bool reusable) {
    if (!connection) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stopping_ && initialized_ && reusable && connection->IsConnected()) {
        idle_.push(std::move(connection));
    } else {
        if (total_ > 0) {
            --total_;
        }
        ++stats_.invalidated;
    }
    condition_.notify_one();
}

std::unique_ptr<RedisConnection> RedisPool::CreateConnected(RedisCommandResult *error) {
    if (!factory_) {
        *error = {.success = false,
                  .error = RedisError::kConnectionUnavailable,
                  .message = "redis connection factory is null"};
        return nullptr;
    }
    std::unique_ptr<RedisConnection> connection = factory_->Create(config_);
    if (!connection) {
        *error = {.success = false,
                  .error = RedisError::kConnectionUnavailable,
                  .message = "redis connection factory returned null"};
        return nullptr;
    }
    *error = connection->Connect();
    return error->ok() ? std::move(connection) : nullptr;
}

}  // namespace chat
