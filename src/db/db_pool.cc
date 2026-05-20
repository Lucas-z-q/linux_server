#include "db/db_pool.h"

#include <iostream>
#include <thread>

namespace chat {

const char* DbPoolErrorToString(DbPoolError error) {
    switch (error) {
        case DbPoolError::kNone: return "None";
        case DbPoolError::kInvalidConfig: return "InvalidConfig";
        case DbPoolError::kNotInitialized: return "NotInitialized";
        case DbPoolError::kStopping: return "Stopping";
        case DbPoolError::kConnectFailed: return "ConnectFailed";
        case DbPoolError::kBorrowTimeout: return "BorrowTimeout";
        case DbPoolError::kHealthCheckFailed: return "HealthCheckFailed";
    }
    return "UnknownError";
}

PooledConnection::PooledConnection(DbPool* pool, std::unique_ptr<DbConnection> conn,
                                   std::chrono::steady_clock::time_point create_time) {
    pool_ = pool;
    conn_ = std::move(conn);
    create_time_ = create_time;
}

PooledConnection::~PooledConnection() {
    if (pool_) {
        pool_->returnConnection(std::move(conn_), create_time_, reusable_);
    }
    pool_ = nullptr;
}

PooledConnection::PooledConnection(PooledConnection&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)), create_time_(other.create_time_), reusable_(other.reusable_) {
    // 转移所有权后，必须将 other 的 pool_ 置空
    // 否则 other 析构时会错误地向连接池归还一个空连接
    other.pool_ = nullptr;
}

PooledConnection& PooledConnection::operator=(PooledConnection&& other) noexcept {
    if (this != &other) {
        // 如果当前对象本身就持有一个连接，先归还回去，避免泄漏
        if (pool_ && conn_) {
            pool_->returnConnection(std::move(conn_), create_time_, reusable_);
        }
        pool_ = other.pool_;
        conn_ = std::move(other.conn_);
        create_time_ = other.create_time_;
        reusable_ = other.reusable_;
        other.pool_ = nullptr;
    }
    return *this;
}

DbConnection* PooledConnection::operator->() noexcept { return conn_.get(); }

DbConnection& PooledConnection::operator*() noexcept { return *conn_; }

PooledConnection::operator bool() const noexcept { return conn_ != nullptr; }

void PooledConnection::markBad() noexcept { reusable_ = false; }

void DbPool::logStats(const std::string& action, DbPoolError error, const std::string& extra) {
    std::cerr << "[db_pool] action=" << action
              << " error=" << DbPoolErrorToString(error)
              << " idle=" << idle_connections_.size()
              << " total=" << total_connections_
              << " waiting=" << stats_.waiting_threads
              << " created=" << stats_.total_created
              << " ping_failed=" << stats_.total_ping_failed
              << " invalidated=" << stats_.total_invalidated
              << " expired=" << stats_.total_expired
              << " timeouts=" << stats_.total_timeouts;
    if (!extra.empty()) {
        std::cerr << " details=" << extra;
    }
    std::cerr << std::endl;
}

DbPool::DbPool(const DbConfig& config)
    : config_(config), connection_factory_(std::make_shared<DefaultDbConnectionFactory>()) {}

DbPool::DbPool(const DbConfig& config, const DbPoolConfig& pool_config)
    : config_(config), pool_config_(pool_config), connection_factory_(std::make_shared<DefaultDbConnectionFactory>()) {}

DbPool::DbPool(const DbConfig& config, const DbPoolConfig& pool_config, std::shared_ptr<IDbConnectionFactory> factory)
    : config_(config),
      pool_config_(pool_config),
      connection_factory_(factory ? factory : std::make_shared<DefaultDbConnectionFactory>()) {}

DbPool::~DbPool() { stop(); }

DbPoolInitResult DbPool::init() {
    // 校验基础数据库配置
    if (config_.host.empty() || config_.port == 0 || config_.username.empty() || config_.database.empty()) {
        std::cerr << "[db_pool] action=init error=invalid_config details=host, port, username, or database is empty" << std::endl;
        return DbPoolInitResult{false, DbPoolError::kInvalidConfig, 0, "host, port, username, or database is empty"};
    }

    // 校验连接池容量配置
    if (pool_config_.min_connections > pool_config_.max_connections) {
        std::cerr << "[db_pool] action=init error=invalid_config details=min_connections > max_connections" << std::endl;
        return DbPoolInitResult{false, DbPoolError::kInvalidConfig, 0, "min_connections > max_connections"};
    }

    if (pool_config_.max_connections <= 0) {
        std::cerr << "[db_pool] action=init error=invalid_config details=max_connections <= 0" << std::endl;
        return DbPoolInitResult{false, DbPoolError::kInvalidConfig, 0, "max_connections <= 0"};
    }

    // 校验借连接超时语义（这里选择严格策略：明确拒绝为 0 的情况，防止用户误配导致无限死循环或瞬间降级）
    if (pool_config_.borrow_timeout_ms == 0) {
        std::cerr << "[db_pool] action=init error=invalid_config details=borrow_timeout_ms == 0" << std::endl;
        return DbPoolInitResult{false, DbPoolError::kInvalidConfig, 0, "borrow_timeout_ms == 0"};
    }

    // 校验重试语义（如果开启了重试，那么重试间隔不能为 0，防止无缝自旋把 CPU 或者对端 MySQL 打死）
    if (pool_config_.connect_retry_count > 0 && pool_config_.connect_retry_delay_ms == 0) {
        std::cerr << "[db_pool] action=init error=invalid_config details=connect_retry_delay_ms == 0" << std::endl;
        return DbPoolInitResult{false, DbPoolError::kInvalidConfig, 0, "connect_retry_delay_ms == 0"};
    }

    // 锁外预创建，避免慢 I/O 阻塞连接池
    std::vector<IdleConnection> temp_connections;
    for (std::size_t i = 0; i < pool_config_.min_connections; ++i) {
        auto res = createConnection();
        if (!res.connection) {
            std::cerr << "[db_pool] action=init error=" << DbPoolErrorToString(res.error)
                      << " mysql_errno=" << res.mysql_error_code
                      << " details=failed to create initial connection: " << res.message << std::endl;
            return DbPoolInitResult{false, res.error, res.mysql_error_code, "failed to create initial connection: " + res.message};
        }
        IdleConnection idle;
        idle.conn = std::move(res.connection);
        idle.create_time = std::chrono::steady_clock::now();
        idle.last_return_time = idle.create_time;
        temp_connections.push_back(std::move(idle));
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& idle : temp_connections) {
        idle_connections_.push(std::move(idle));
        total_connections_++;
        stats_.total_created++;  // 计入真正的成功初始化建连数
    }

    initialized_ = true;
    return DbPoolInitResult{true, DbPoolError::kNone, 0, "init success"};
}

BorrowConnectionResult DbPool::borrow() {
    std::unique_lock<std::mutex> lock(mutex_);
    bool encountered_health_check = false;

    while (true) {
        // 第一种情况：未初始化或连接池已经停机
        if (!initialized_) {
            logStats("borrow", DbPoolError::kNotInitialized);
            return BorrowConnectionResult{std::nullopt, DbPoolError::kNotInitialized, 0, "DbPool is not initialized"};
        }
        if (stopping_) {
            logStats("borrow", DbPoolError::kStopping);
            return BorrowConnectionResult{std::nullopt, DbPoolError::kStopping, 0, "DbPool is stopping"};
        }

        // 第二种情况：有空闲连接
        if (!idle_connections_.empty()) {
            auto idle_wrapper = std::move(idle_connections_.front());
            idle_connections_.pop();

            // 释放锁，在锁外做健康检查，避免阻塞其他竞争获取连接的线程
            lock.unlock();

            auto now = std::chrono::steady_clock::now();

            if (isExpired(idle_wrapper, now)) {
                // 在锁外显式断开连接，避免阻塞其他线程
                auto conn_to_close = std::move(idle_wrapper.conn);
                conn_to_close.reset();

                // 重新获取锁，并扣除总连接数
                lock.lock();
                stats_.total_expired++;
                total_connections_--;
                cv_.notify_one();
                continue;
            }

            bool is_healthy = idle_wrapper.conn->ping();

            // 重新获取锁
            lock.lock();

            // 锁外检查期间可能发生了停机
            if (stopping_) {
                total_connections_--;
                cv_.notify_all();

                logStats("borrow", DbPoolError::kStopping);

                auto conn_to_close = std::move(idle_wrapper.conn);
                lock.unlock();
                conn_to_close.reset();  // 在锁外显式触发析构并物理断开

                return BorrowConnectionResult{std::nullopt, DbPoolError::kStopping, 0, "DbPool is stopping"};
            }

            if (is_healthy) {
                return BorrowConnectionResult{PooledConnection(this, std::move(idle_wrapper.conn), idle_wrapper.create_time), DbPoolError::kNone, 0, ""};
            }

            // ping 失败处理策略（采用 continue 循环模式）：
            // 仅对真实失效的连接增加重建/重连统计
            stats_.total_ping_failed++;
            encountered_health_check = true;
            logStats("borrow", DbPoolError::kHealthCheckFailed, "idle connection ping failed");

            // 及时释放名额，并唤醒其他可能正在等待新建连接名额的线程
            total_connections_--;
            cv_.notify_one();

            // 在锁外安全地销毁失效连接
            auto conn_to_close = std::move(idle_wrapper.conn);
            lock.unlock();
            conn_to_close.reset();  // 在锁外显式触发析构并物理断开
            lock.lock();

            // 继续循环：若队列还有连接则继续 ping；
            // 若队列已空，下一次循环必然进入第三阶段尝试 createConnection()。
            // createConnection() 内置了重试次数限制，若失败会直接返回 nullopt 退出，不会导致无限循环。
            continue;
        }

        // 第三种情况：没有空闲连接，但还没达到最大连接数
        if (total_connections_ < pool_config_.max_connections) {
            // 先预留一个连接名额，防止多个线程并发进入这个分支导致连接数超标
            total_connections_++;

            // 释放锁，在锁外进行耗时的底层建连操作
            lock.unlock();

            auto new_conn_res = createConnection();

            // 重新获取锁，检查停机状态并更新统计
            lock.lock();

            if (stopping_) {
                total_connections_--;
                cv_.notify_all();  // 释放了名额，且因为是停机，唤醒所有等待者

                logStats("borrow", DbPoolError::kStopping);

                // 统一风格，在锁外释放刚建好的新连接
                auto conn_to_close = std::move(new_conn_res.connection);
                lock.unlock();
                conn_to_close.reset();

                return BorrowConnectionResult{std::nullopt, DbPoolError::kStopping, 0, "DbPool is stopping"};
            }

            if (new_conn_res.connection) {
                stats_.total_created++;  // 只有真正建立成功并被池子接纳才计入创建统计
                return BorrowConnectionResult{PooledConnection(this, std::move(new_conn_res.connection), std::chrono::steady_clock::now()), DbPoolError::kNone, 0, ""};
            } else {
                // 建连失败，必须退回之前预留的名额
                total_connections_--;
                cv_.notify_one();     // 释放了名额，唤醒其他可能正在等待的线程
                
                DbPoolError final_err = encountered_health_check ? DbPoolError::kHealthCheckFailed : new_conn_res.error;
                logStats("borrow", final_err, "mysql_errno=" + std::to_string(new_conn_res.mysql_error_code) + " details=" + new_conn_res.message);
                return BorrowConnectionResult{std::nullopt, final_err, new_conn_res.mysql_error_code, new_conn_res.message};
            }
        }

        // 第四种情况：已经达到最大连接数，必须等待
        stats_.waiting_threads++;
        bool wait_success = cv_.wait_for(lock, std::chrono::milliseconds(pool_config_.borrow_timeout_ms), [this]() {
            return stopping_ || !idle_connections_.empty() || total_connections_ < pool_config_.max_connections;
        });
        stats_.waiting_threads--;

        // 被唤醒后检查是不是停机了
        if (stopping_) {
            logStats("borrow", DbPoolError::kStopping);
            return BorrowConnectionResult{std::nullopt, DbPoolError::kStopping, 0, "DbPool is stopping"};
        }

        // 如果因为超时被唤醒，并且依然没有空闲连接可用
        if (!wait_success && idle_connections_.empty()) {
            stats_.total_timeouts++;
            DbPoolError final_err = encountered_health_check ? DbPoolError::kHealthCheckFailed : DbPoolError::kBorrowTimeout;
            logStats("borrow", final_err, "borrow connection timeout");
            return BorrowConnectionResult{std::nullopt, final_err, 0, "borrow connection timeout"};
        }

        // 正常被归还唤醒，会自动重新回到 while 循环开始，尝试获取刚归还的连接
    }
}

void DbPool::returnConnection(std::unique_ptr<DbConnection> conn, std::chrono::steady_clock::time_point create_time,
                              bool reusable) {
    if (!conn) {
        return;
    }

    // 锁外执行耗时的网络 I/O 健康检查；若已被业务明确标记不可复用，直接跳过 ping
    bool is_healthy = reusable && conn->ping();  // DbConnection::ping() 内部已包含 isConnected() 检查

    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_ || !is_healthy) {
        if (!stopping_ && !is_healthy) {
            stats_.total_invalidated++;
            logStats("return", DbPoolError::kHealthCheckFailed, reusable ? "connection ping failed on return" : "connection marked bad by user");
        } else if (stopping_) {
            logStats("return", DbPoolError::kStopping, "discarding returned connection during pool shutdown");
        }

        total_connections_--;
        cv_.notify_one();  // 释放了连接名额，唤醒等待的线程

        // 释放锁，在锁外安全地进行底层的物理关闭操作
        lock.unlock();
        conn->close();
        return;
    }

    IdleConnection idle;
    idle.conn = std::move(conn);
    idle.create_time = create_time;
    idle.last_return_time = std::chrono::steady_clock::now();
    idle_connections_.push(std::move(idle));
    cv_.notify_one();
}

CreateConnectionResult DbPool::createConnection() {
    std::uint32_t max_attempts = pool_config_.connect_retry_count + 1;
    CreateConnectionResult res;

    for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt) {
        auto conn = connection_factory_->createConnection(config_);
        if (!conn) {
            res.connection = nullptr;
            res.error = DbPoolError::kConnectFailed;
            res.mysql_error_code = 0;
            res.message = "connection factory returned null";
            return res;
        }
        auto conn_res = conn->connect();
        if (conn_res.success) {
            res.connection = std::move(conn);
            res.error = DbPoolError::kNone;
            res.mysql_error_code = 0;
            res.message.clear();
            return res;
        }

        res.error = DbPoolError::kConnectFailed;
        res.mysql_error_code = conn_res.error_code;
        res.message = conn_res.error_message;

        if (attempt < max_attempts - 1 && pool_config_.connect_retry_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pool_config_.connect_retry_delay_ms));
        }
    }
    return res;
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

bool DbPool::isExpired(const IdleConnection& idle, std::chrono::steady_clock::time_point now) {
    auto duration = now - idle.last_return_time;
    auto life_time = now - idle.create_time;
    return duration > std::chrono::milliseconds(pool_config_.idle_timeout_ms) ||
           life_time > std::chrono::milliseconds(pool_config_.max_lifetime_ms);
}

}  // namespace chat
