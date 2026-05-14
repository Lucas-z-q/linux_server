#include "db/db_pool.h"

namespace chat {

PooledConnection::PooledConnection(DbPool* pool, std::unique_ptr<DbConnection> conn) {
    pool_ = pool;
    conn_ = std::move(conn);
}

PooledConnection::~PooledConnection() {
    if (pool_) {
        pool_->returnConnection(std::move(conn_));
    }
    pool_ = nullptr;
}

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) {
    // 转移所有权后，必须将 other 的 pool_ 置空
    // 否则 other 析构时会错误地向连接池归还一个空连接
    other.pool_ = nullptr;
}

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        // 如果当前对象本身就持有一个连接，先归还回去，避免泄漏
        if (pool_ && conn_) {
            pool_->returnConnection(std::move(conn_));
        }
        pool_ = other.pool_;
        conn_ = std::move(other.conn_);
        other.pool_ = nullptr;
    }
    return *this;
}

DbConnection* PooledConnection::operator->() noexcept { return conn_.get(); }

DbConnection& PooledConnection::operator*() noexcept { return *conn_; }

PooledConnection::operator bool() const noexcept { return conn_ != nullptr; }

DbPool::DbPool(const DbConfig& config) : config_(config) {}

DbPool::DbPool(const DbConfig& config, const DbPoolConfig& pool_config) : config_(config), pool_config_(pool_config) {}

DbPool::~DbPool() { stop(); }

bool DbPool::init() {
    if (config_.host.empty() || config_.port == 0 || config_.username.empty() || config_.database.empty())
        return false;

    if (pool_config_.min_connections > pool_config_.max_connections)
        return false;

    if (pool_config_.max_connections <= 0)
        return false;

    // 锁外预创建，避免慢 I/O 阻塞连接池
    std::vector<IdleConnection> temp_connections;
    for (std::size_t i = 0; i < pool_config_.min_connections; ++i) {
        std::unique_ptr<DbConnection> conn = std::make_unique<DbConnection>(config_);

        DbConnectionResult res = conn->connect();
        if (!res.ok()) {
            return false;  // 建连失败时，temp_connections 超出作用域会自动安全销毁已建连接
        }
        IdleConnection idle;
        idle.conn = std::move(conn);
        idle.create_time = std::chrono::steady_clock::now();
        idle.last_return_time = idle.create_time;
        temp_connections.push_back(std::move(idle));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& idle : temp_connections) {
        idle_connections_.push(std::move(idle));
        total_connections_++;
    }

    initialized_ = true;
    return true;
}

std::optional<PooledConnection> DbPool::borrow() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        // 第一种情况：未初始化或连接池已经停机
        if (!initialized_ || stopping_) {
            return std::nullopt;
        }

        // 第二种情况：有空闲连接
        if (!idle_connections_.empty()) {
            auto idle_wrapper = std::move(idle_connections_.front());
            idle_connections_.pop();

            // 释放锁，在锁外做健康检查，避免阻塞其他竞争获取连接的线程
            lock.unlock();

            if (idle_wrapper.conn->ping()) {
                return PooledConnection(this, std::move(idle_wrapper.conn));
            }

            // ping 失败，重新获取锁，减去总连接数计数并丢弃该连接
            lock.lock();
            total_connections_--;
            stats_.total_reconnected++;
            // 继续循环（下一次可能会走到第三种情况去建连）
            continue;
        }

        // 第三种情况：没有空闲连接，但还没达到最大连接数
        if (total_connections_ < pool_config_.max_connections) {
            // 先预留一个连接名额，防止多个线程并发进入这个分支导致连接数超标
            total_connections_++;
            stats_.total_created++;

            // 释放锁，在锁外进行耗时的底层建连操作
            lock.unlock();

            auto new_conn = std::make_unique<DbConnection>(config_);
            DbConnectionResult res = new_conn->connect();

            if (res.ok()) {
                return PooledConnection(this, std::move(new_conn));
            } else {
                // 建连失败，必须退回之前预留的名额
                lock.lock();
                total_connections_--;
                return std::nullopt;  // 建连失败直接返回空，由业务端决定是否重试
            }
        }

        // 第四种情况：已经达到最大连接数，必须等待
        stats_.waiting_threads++;
        bool wait_success = cv_.wait_for(lock, std::chrono::milliseconds(pool_config_.borrow_timeout_ms),
                                         [this]() { return stopping_ || !idle_connections_.empty(); });
        stats_.waiting_threads--;

        // 被唤醒后检查是不是停机了
        if (stopping_) {
            return std::nullopt;
        }

        // 如果因为超时被唤醒，并且依然没有空闲连接可用
        if (!wait_success && idle_connections_.empty()) {
            stats_.total_timeouts++;
            return std::nullopt;
        }

        // 正常被归还唤醒，会自动重新回到 while 循环开始，尝试获取刚归还的连接
    }
}

void DbPool::returnConnection(std::unique_ptr<DbConnection> conn) {
    if (!conn) {
        return;
    }

    // 1. 锁外执行耗时的网络 I/O 健康检查，防止阻塞连接池其他操作
    bool is_healthy = conn->ping();  // DbConnection::ping() 内部已包含 isConnected() 检查

    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_ || !is_healthy) {
        total_connections_--;

        // 2. 释放锁，在锁外安全地进行底层的物理关闭操作
        lock.unlock();
        conn->close();
        return;
    }

    IdleConnection idle;
    idle.conn = std::move(conn);
    idle.create_time = std::chrono::steady_clock::now();
    idle.last_return_time = idle.create_time;
    idle_connections_.push(std::move(idle));
    cv_.notify_one();
}

void DbPool::stop() {
    std::queue<IdleConnection> connections_to_close;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 如果已经 stopping_ == true，直接返回，保证幂等性
        if (stopping_) {
            return;
        }
        stopping_ = true;

        // 唤醒所有等待中的 borrow()
        cv_.notify_all();

        // 取出所有空闲连接转移到本地队列中
        idle_connections_.swap(connections_to_close);

        // 同步扣除已丢弃的空闲连接总数（被借出的会在归还时因为 stopping_==true 被扣除）
        total_connections_ -= connections_to_close.size();
    }  // 离开此大括号后，lock 被析构，解锁

    // 在锁外逐个关闭连接，避免网络 I/O 阻塞其他想要归还连接的线程
    while (!connections_to_close.empty()) {
        auto idle_wrapper = std::move(connections_to_close.front());
        connections_to_close.pop();
        if (idle_wrapper.conn) {
            idle_wrapper.conn->close();
        }
    }
}

DbPool::Stats DbPool::getStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats current_stats = stats_;
    current_stats.current_idle = idle_connections_.size();
    current_stats.current_total = total_connections_;
    return current_stats;
}

}  // namespace chat
