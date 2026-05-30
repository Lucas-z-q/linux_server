#include "db/db_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace chat {
namespace {

// 从环境变量辅助加载配置，避免硬编码密码。
// 与 UserRepository 中的逻辑类似。
DbConfig GetTestDbConfig() {
    DbConfig config;
    if (const char* host = std::getenv("CHAT_DB_HOST"))
        config.host = host;
    if (const char* port = std::getenv("CHAT_DB_PORT"))
        config.port = std::stoi(port);
    if (const char* user = std::getenv("CHAT_DB_USER"))
        config.username = user;
    if (const char* pwd = std::getenv("CHAT_DB_PASSWORD"))
        config.password = pwd;
    if (const char* db = std::getenv("CHAT_DB_NAME"))
        config.database = db;
    return config;
}

class FakeDbConnection : public DbConnection {
   public:
    FakeDbConnection(const DbConfig& config, std::size_t id, bool should_connect_succeed, bool should_ping_succeed)
        : DbConnection(config),
          id_(id),
          connect_succeed_(should_connect_succeed),
          ping_succeed_(should_ping_succeed),
          connected_(false) {}

    DbConnectionResult connect() override {
        if (connect_succeed_) {
            connected_ = true;
            return DbConnectionResult{true};
        }
        return DbConnectionResult{false, 2003, "Can't connect to MySQL server"};
    }

    void close() noexcept override { connected_ = false; }

    bool ping() noexcept override { return connected_ && ping_succeed_; }

    bool isConnected() const noexcept override { return connected_; }

    void setPingSucceed(bool succeed) noexcept { ping_succeed_ = succeed; }

    std::size_t id() const noexcept { return id_; }

   private:
    std::size_t id_;
    bool connect_succeed_;
    bool ping_succeed_;
    bool connected_;
};

class FakeDbConnectionFactory : public IDbConnectionFactory {
   public:
    FakeDbConnectionFactory(bool connect_succeed, bool ping_succeed)
        : connect_succeed_(connect_succeed), ping_succeed_(ping_succeed) {}

    std::unique_ptr<DbConnection> createConnection(const DbConfig& config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t id = ++created_count_;
        auto conn = std::make_unique<FakeDbConnection>(config, id, connect_succeed_, ping_succeed_);
        last_created_connection_ = conn.get();
        return conn;
    }

    void setConnectSucceed(bool succeed) {
        std::lock_guard<std::mutex> lock(mutex_);
        connect_succeed_ = succeed;
    }

    void setPingSucceed(bool succeed) {
        std::lock_guard<std::mutex> lock(mutex_);
        ping_succeed_ = succeed;
    }

    FakeDbConnection* getLastCreatedConnection() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_created_connection_;
    }

    std::size_t createdCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return created_count_;
    }

   private:
    mutable std::mutex mutex_;
    bool connect_succeed_;
    bool ping_succeed_;
    FakeDbConnection* last_created_connection_ = nullptr;
    std::size_t created_count_ = 0;
};

DbConfig MakeValidMockConfig() {
    DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3306;
    config.username = "root";
    config.database = "test";
    return config;
}

class DbPoolTest : public ::testing::Test {
   protected:
    void SetUp() override { config_ = GetTestDbConfig(); }

    DbConfig config_;
};

// 语义测试 1：stop() 可重复调用（幂等性）
TEST_F(DbPoolTest, StopIsIdempotent) {
    DbPool pool(config_);
    pool.stop();
    pool.stop();  // 再次调用不应崩溃或死锁
    SUCCEED();
}

TEST_F(DbPoolTest, InitSuccessPreCreatesMinConnections) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 2;
    pool_config.max_connections = 4;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);

    auto init_res = pool.init();
    ASSERT_TRUE(init_res.success);
    EXPECT_EQ(init_res.error, DbPoolError::kNone);

    auto stats = pool.getStats();
    EXPECT_EQ(stats.current_idle, 2);
    EXPECT_EQ(stats.current_total, 2);
    EXPECT_EQ(stats.total_created, 2);
    EXPECT_EQ(factory->createdCount(), 2);
}

TEST_F(DbPoolTest, InitFailsWhenConfigMissing) {
    DbConfig bad_config;
    bad_config.host = "127.0.0.1";
    bad_config.port = 3306;
    bad_config.username.clear();
    bad_config.database.clear();

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(bad_config, DbPoolConfig{}, factory);

    auto init_res = pool.init();
    EXPECT_FALSE(init_res.success);
    EXPECT_EQ(init_res.error, DbPoolError::kInvalidConfig);
    EXPECT_EQ(factory->createdCount(), 0);
}

// 语义测试 2：未提供正确配置(相当于未成功 init) 或 stop 后，borrow 应该失败
TEST_F(DbPoolTest, BorrowFailsBeforeInitOrAfterStop) {
    DbConfig bad_config;
    bad_config.host = "invalid_host";
    DbPool pool(bad_config);

    // 未成功 init 时，如果配置非法，borrow 会由于底层建连失败而返回空
    auto conn1 = pool.borrow();
    EXPECT_FALSE(conn1.ok());
    EXPECT_EQ(conn1.error, DbPoolError::kNotInitialized);

    // 调用 init 失败
    auto init_res = pool.init();
    EXPECT_FALSE(init_res.success);
    EXPECT_EQ(init_res.error, DbPoolError::kInvalidConfig);

    // 此时 borrow 应由于未完成初始化（建连失败）返回 kNotInitialized
    auto conn1_post_init = pool.borrow();
    EXPECT_FALSE(conn1_post_init.ok());
    EXPECT_EQ(conn1_post_init.error, DbPoolError::kNotInitialized);

    pool.stop();
    // 停机后，由于还是未成功 init，所以还是返回 kNotInitialized
    auto conn2 = pool.borrow();
    EXPECT_FALSE(conn2.ok());
    EXPECT_EQ(conn2.error, DbPoolError::kNotInitialized);

    // 测试成功初始化后的停机情况 (通过设置 min_connections = 0 绕过真实数据库建连)
    DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";
    DbPoolConfig pool_config;
    pool_config.min_connections = 0;
    DbPool good_pool(mock_config, pool_config);

    auto good_init = good_pool.init();
    EXPECT_TRUE(good_init.success);

    good_pool.stop();
    auto conn3 = good_pool.borrow();
    EXPECT_FALSE(conn3.ok());
    EXPECT_EQ(conn3.error, DbPoolError::kStopping);
}

TEST_F(DbPoolTest, BorrowReturnsConnectionToIdleQueue) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 2;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    auto before = pool.getStats();
    ASSERT_EQ(before.current_idle, 1);

    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        auto during = pool.getStats();
        EXPECT_EQ(during.current_idle, 0);
        EXPECT_EQ(during.current_total, 1);
    }

    auto after = pool.getStats();
    EXPECT_EQ(after.current_idle, 1);
    EXPECT_EQ(after.current_total, 1);
}

TEST_F(DbPoolTest, MultipleBorrowsReuseSameConnection) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 1;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    std::size_t first_id = 0;
    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        auto* fake = dynamic_cast<FakeDbConnection*>(&(**conn.connection));
        ASSERT_NE(fake, nullptr);
        first_id = fake->id();
    }

    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        auto* fake = dynamic_cast<FakeDbConnection*>(&(**conn.connection));
        ASSERT_NE(fake, nullptr);
        EXPECT_EQ(fake->id(), first_id);
    }

    EXPECT_EQ(factory->createdCount(), 1);
}

// 语义测试 3：借出连接后析构能自动归还
TEST_F(DbPoolTest, DestructorReturnsConnection) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 2;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    auto initial_stats = pool.getStats();

    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());

        // 借出期间，空闲数量应该减 1
        auto stats_during = pool.getStats();
        EXPECT_EQ(stats_during.current_idle, initial_stats.current_idle - 1);
    }  // conn 离开作用域触发 PooledConnection 析构

    // 析构后，连接应该被自动还回，空闲数量恢复
    auto stats_after = pool.getStats();
    EXPECT_EQ(stats_after.current_idle, initial_stats.current_idle);
}

// 语义测试 4：连续 borrow 不超过 max_connections
TEST_F(DbPoolTest, MaxConnectionsLimitAndBlock) {
    DbPoolConfig pool_config;
    pool_config.max_connections = 10;
    pool_config.min_connections = 1;
    pool_config.borrow_timeout_ms = 1000;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    const int max_conns = 10;
    std::vector<PooledConnection> conns;

    for (int i = 0; i < max_conns; ++i) {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok()) << "Failed to borrow connection " << i;
        conns.push_back(std::move(*conn.connection));
    }

    // 此时已经借空，达到了总数上限，空闲为 0
    EXPECT_EQ(pool.getStats().current_total, max_conns);
    EXPECT_EQ(pool.getStats().current_idle, 0);

    // 使用异步线程在 100 毫秒后归还一个连接
    std::thread return_thread([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        conns.pop_back();  // 弹出最后一个连接，触发析构自动归还
    });

    auto start = std::chrono::steady_clock::now();
    // 由于达到上限，这里会阻塞在 cv_.wait_for，直到上面的线程归还
    auto blocked_conn = pool.borrow();
    auto end = std::chrono::steady_clock::now();

    EXPECT_TRUE(blocked_conn.ok());
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(duration_ms, 50);  // 证明确实发生了阻塞等待，没有盲目超建连接

    return_thread.join();
}

TEST_F(DbPoolTest, MaxConnectionsLimitAndWaitUsesFakeConnections) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 0;
    pool_config.max_connections = 2;
    pool_config.borrow_timeout_ms = 1000;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    std::vector<PooledConnection> held;
    for (std::size_t i = 0; i < pool_config.max_connections; ++i) {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        held.push_back(std::move(*conn.connection));
    }
    EXPECT_EQ(pool.getStats().current_total, pool_config.max_connections);
    EXPECT_EQ(factory->createdCount(), pool_config.max_connections);

    std::thread return_thread([&held]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        held.pop_back();
    });

    auto start = std::chrono::steady_clock::now();
    auto waited = pool.borrow();
    auto elapsed = std::chrono::steady_clock::now() - start;
    return_thread.join();

    EXPECT_TRUE(waited.ok());
    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 40);
    EXPECT_EQ(factory->createdCount(), pool_config.max_connections);
}

// 语义测试 5：发生 move 后，原来的空壳不应该向连接池归还 nullptr
TEST_F(DbPoolTest, MoveSemanticsDoesNotDoubleReturn) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 2;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    auto initial_stats = pool.getStats();

    {
        auto conn1 = pool.borrow();
        ASSERT_TRUE(conn1.ok());
        PooledConnection conn2 = std::move(*conn1.connection);  // 移动构造
        PooledConnection conn3;
        conn3 = std::move(conn2);  // 移动赋值
    }  // 依次析构，conn1 和 conn2 是空壳，conn3 持有真实连接

    EXPECT_EQ(pool.getStats().current_total, initial_stats.current_total);
}

// 语义测试 6：连接拒绝，返回 kConnectFailed
TEST_F(DbPoolTest, ConnectFailedErrorCase) {
    DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";

    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.connect_retry_count = 0;  // 不重试

    // 注入总是连接失败的 factory
    auto factory = std::make_shared<FakeDbConnectionFactory>(false, false);
    DbPool pool(mock_config, pool_config, factory);

    auto init_res = pool.init();
    EXPECT_FALSE(init_res.success);
    EXPECT_EQ(init_res.error, DbPoolError::kConnectFailed);
}

// 语义测试 7：获取连接超时，返回 kBorrowTimeout (使用 mock 离线测试)
TEST_F(DbPoolTest, BorrowTimeoutErrorCase) {
    DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";

    DbPoolConfig pool_config;
    pool_config.max_connections = 1;
    pool_config.min_connections = 1;
    pool_config.borrow_timeout_ms = 50;

    // 注入总是能成功建连和 ping 的 factory
    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(mock_config, pool_config, factory);

    auto init_res = pool.init();
    ASSERT_TRUE(init_res.success);

    // 借出唯一连接
    auto conn1 = pool.borrow();
    ASSERT_TRUE(conn1.ok());

    // 再次借用时，由于没有可用连接且已达上限，会超时并返回 kBorrowTimeout
    auto conn2 = pool.borrow();
    EXPECT_FALSE(conn2.ok());
    EXPECT_EQ(conn2.error, DbPoolError::kBorrowTimeout);
    EXPECT_EQ(pool.getStats().total_timeouts, 1);
}

TEST_F(DbPoolTest, StopWakesWaitingBorrowers) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 1;
    pool_config.borrow_timeout_ms = 5000;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    auto held = pool.borrow();
    ASSERT_TRUE(held.ok());

    std::atomic<bool> waiter_started{false};
    BorrowConnectionResult result;
    std::thread waiter([&]() {
        waiter_started.store(true);
        result = pool.borrow();
    });

    while (!waiter_started.load()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto start = std::chrono::steady_clock::now();
    pool.stop();
    waiter.join();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error, DbPoolError::kStopping);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000);
}

// 语义测试 8：健康检查失败，并且无法重建，返回 kHealthCheckFailed
TEST_F(DbPoolTest, HealthCheckFailedErrorCase) {
    DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";

    DbPoolConfig pool_config;
    pool_config.max_connections = 1;
    pool_config.min_connections = 1;
    pool_config.connect_retry_count = 0;

    // 1. 初始化时 mock 连接成功
    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(mock_config, pool_config, factory);
    auto init_res = pool.init();
    ASSERT_TRUE(init_res.success);

    // 2. 借出连接，记录其 raw 指针，然后归还连接
    FakeDbConnection* raw_conn_ptr = nullptr;
    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        raw_conn_ptr = dynamic_cast<FakeDbConnection*>(&(**conn.connection));
    }  // 成功归还

    ASSERT_NE(raw_conn_ptr, nullptr);

    // 3. 模拟空闲检查时 ping 失败
    raw_conn_ptr->setPingSucceed(false);

    // 4. 让 factory 下次建连失败，导致重建连接一定会失败
    factory->setConnectSucceed(false);

    // 5. 再次借用连接：
    // - pop 出空闲连接，检测到其 ping() 失败，丢弃它。
    // - 尝试重建，但重建失败。
    // - 最终返回 kHealthCheckFailed！
    auto conn2 = pool.borrow();
    EXPECT_FALSE(conn2.ok());
    EXPECT_EQ(conn2.error, DbPoolError::kHealthCheckFailed);
}

TEST_F(DbPoolTest, HealthCheckFailureRebuildsConnectionWhenFactoryCanRecover) {
    DbPoolConfig pool_config;
    pool_config.max_connections = 1;
    pool_config.min_connections = 1;
    pool_config.connect_retry_count = 0;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    std::size_t first_id = 0;
    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.ok());
        auto* fake = dynamic_cast<FakeDbConnection*>(&(**conn.connection));
        ASSERT_NE(fake, nullptr);
        first_id = fake->id();
    }

    auto* returned = factory->getLastCreatedConnection();
    ASSERT_NE(returned, nullptr);
    returned->setPingSucceed(false);

    auto rebuilt = pool.borrow();
    ASSERT_TRUE(rebuilt.ok());
    auto* fake = dynamic_cast<FakeDbConnection*>(&(**rebuilt.connection));
    ASSERT_NE(fake, nullptr);
    EXPECT_NE(fake->id(), first_id);

    auto stats = pool.getStats();
    EXPECT_EQ(stats.total_ping_failed, 1);
    EXPECT_EQ(stats.total_created, 2);
    EXPECT_EQ(stats.current_total, 1);
}

// 语义测试 9：如果自定义连接工厂返回空指针，返回 kConnectFailed
TEST_F(DbPoolTest, FactoryReturnsNullErrorCase) {
    DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";

    DbPoolConfig pool_config;
    pool_config.min_connections = 1;

    class NullDbConnectionFactory : public IDbConnectionFactory {
       public:
        std::unique_ptr<DbConnection> createConnection(const DbConfig&) override { return nullptr; }
    };

    auto factory = std::make_shared<NullDbConnectionFactory>();
    DbPool pool(mock_config, pool_config, factory);

    auto init_res = pool.init();
    EXPECT_FALSE(init_res.success);
    EXPECT_EQ(init_res.error, DbPoolError::kConnectFailed);
}

TEST_F(DbPoolTest, ConcurrentBorrowReturnStressDoesNotExceedMaxConnections) {
    DbPoolConfig pool_config;
    pool_config.min_connections = 2;
    pool_config.max_connections = 4;
    pool_config.borrow_timeout_ms = 1000;

    auto factory = std::make_shared<FakeDbConnectionFactory>(true, true);
    DbPool pool(MakeValidMockConfig(), pool_config, factory);
    ASSERT_TRUE(pool.init().success);

    constexpr int kWorkerCount = 12;
    constexpr int kIterations = 25;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::atomic<std::size_t> max_seen_total{0};
    std::vector<std::thread> workers;

    for (int i = 0; i < kWorkerCount; ++i) {
        workers.emplace_back([&]() {
            for (int j = 0; j < kIterations; ++j) {
                auto conn = pool.borrow();
                if (!conn.ok()) {
                    failure_count++;
                    continue;
                }
                success_count++;
                auto stats = pool.getStats();
                std::size_t observed = stats.current_total;
                std::size_t previous = max_seen_total.load();
                while (observed > previous && !max_seen_total.compare_exchange_weak(previous, observed)) {
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_EQ(failure_count.load(), 0);
    EXPECT_EQ(success_count.load(), kWorkerCount * kIterations);
    EXPECT_LE(max_seen_total.load(), pool_config.max_connections);
    EXPECT_LE(factory->createdCount(), pool_config.max_connections);
    EXPECT_LE(pool.getStats().current_total, pool_config.max_connections);
}

}  // namespace
}  // namespace chat
