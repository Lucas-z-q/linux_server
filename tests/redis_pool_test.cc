#include "redis/redis_pool.h"

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

#include "fake_redis_client.h"

namespace {

chat::RedisConfig MakeConfig(std::size_t size) {
    chat::RedisConfig config;
    config.pool_size = size;
    config.command_timeout_ms = 30;
    return config;
}

void TestInitBorrowAndTimeout() {
    auto factory = std::make_shared<chat::test::FakeRedisConnectionFactory>();
    chat::RedisPool pool(MakeConfig(1), factory);
    assert(pool.Init().ok());
    assert(pool.GetStats().idle == 1);

    auto held = pool.Borrow();
    assert(held.ok());
    auto blocked = pool.Borrow();
    assert(!blocked.ok());
    assert(blocked.error == chat::RedisError::kTimeout);
}

void TestInitFailure() {
    auto factory = std::make_shared<chat::test::FakeRedisConnectionFactory>();
    factory->SetConnectResult(chat::test::RedisFailure(chat::RedisError::kConnectionUnavailable, "cannot connect"));
    chat::RedisPool pool(MakeConfig(2), factory);
    assert(!pool.Init().ok());
}

void TestBadConnectionIsReplaced() {
    auto factory = std::make_shared<chat::test::FakeRedisConnectionFactory>();
    chat::RedisPool pool(MakeConfig(1), factory);
    assert(pool.Init().ok());
    {
        auto borrowed = pool.Borrow();
        assert(borrowed.ok());
        borrowed.connection->MarkBad();
    }
    auto replacement = pool.Borrow();
    assert(replacement.ok());
    assert(factory->created_count() == 2);
    assert(pool.GetStats().invalidated == 1);
}

void TestConcurrentBorrow() {
    chat::RedisConfig config = MakeConfig(4);
    config.command_timeout_ms = 1000;
    auto factory = std::make_shared<chat::test::FakeRedisConnectionFactory>();
    chat::RedisPool pool(config, factory);
    assert(pool.Init().ok());

    std::vector<std::thread> workers;
    for (int i = 0; i < 12; ++i) {
        workers.emplace_back([&pool]() {
            for (int j = 0; j < 20; ++j) {
                auto borrowed = pool.Borrow();
                assert(borrowed.ok());
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }
    assert(pool.GetStats().idle == 4);
}

}  // namespace

int main() {
    TestInitBorrowAndTimeout();
    TestInitFailure();
    TestBadConnectionIsReplaced();
    TestConcurrentBorrow();
    std::cout << "[PASS] redis pool tests passed\n";
    return 0;
}
