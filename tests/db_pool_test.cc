#include "db/db_pool.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
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

// 语义测试 2：未提供正确配置(相当于未成功 init) 或 stop 后，borrow 应该失败
TEST_F(DbPoolTest, BorrowFailsBeforeInitOrAfterStop) {
    DbConfig bad_config;
    bad_config.host = "invalid_host";
    DbPool pool(bad_config);

    // 未成功 init 时，如果配置非法，borrow 会由于底层建连失败而返回空
    auto conn1 = pool.borrow();
    EXPECT_FALSE(conn1.has_value());

    pool.stop();
    // 停机后，borrow 应该被立刻拦截并返回空
    auto conn2 = pool.borrow();
    EXPECT_FALSE(conn2.has_value());
}

// 语义测试 3：借出连接后析构能自动归还
TEST_F(DbPoolTest, DestructorReturnsConnection) {
    DbPool pool(config_);
    if (!pool.init()) {
        GTEST_SKIP() << "Database not available or config missing, skipping real connection test.";
    }

    auto initial_stats = pool.getStats();

    {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.has_value());

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

    DbPool pool(config_, pool_config);
    if (!pool.init()) {
        GTEST_SKIP() << "Database not available or config missing, skipping real connection test.";
    }

    const int max_conns = 10;
    std::vector<PooledConnection> conns;

    for (int i = 0; i < max_conns; ++i) {
        auto conn = pool.borrow();
        ASSERT_TRUE(conn.has_value()) << "Failed to borrow connection " << i;
        conns.push_back(std::move(*conn));
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

    EXPECT_TRUE(blocked_conn.has_value());
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    EXPECT_GE(duration_ms, 50);  // 证明确实发生了阻塞等待，没有盲目超建连接

    return_thread.join();
}

// 语义测试 5：发生 move 后，原来的空壳不应该向连接池归还 nullptr
TEST_F(DbPoolTest, MoveSemanticsDoesNotDoubleReturn) {
    DbPool pool(config_);
    if (!pool.init())
        GTEST_SKIP();

    auto initial_stats = pool.getStats();

    {
        auto conn1 = pool.borrow();
        PooledConnection conn2 = std::move(*conn1);  // 移动构造
        PooledConnection conn3;
        conn3 = std::move(conn2);  // 移动赋值
    }  // 依次析构，conn1 和 conn2 是空壳，conn3 持有真实连接

    EXPECT_EQ(pool.getStats().current_total, initial_stats.current_total);
}

}  // namespace
}  // namespace chat
