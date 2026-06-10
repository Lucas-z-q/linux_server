#include "cache/redis_rate_limiter.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

#include "fake_redis_client.h"

namespace {

void TestFixedWindowAndExpiry() {
    chat::test::FakeRedisClient redis;
    std::int64_t now = 120;
    chat::RedisRateLimiter limiter(&redis, {}, [&now]() { return now; });

    assert(limiter.Allow("login", "127.0.0.1", 2, 60).allowed);
    assert(limiter.Allow("login", "127.0.0.1", 2, 60).allowed);
    const chat::RateLimitResult rejected = limiter.Allow("login", "127.0.0.1", 2, 60);
    assert(!rejected.allowed);
    assert(rejected.retry_after_seconds == 60);

    now = 180;
    assert(limiter.Allow("login", "127.0.0.1", 2, 60).allowed);
}

void TestConcurrentCountIsAtomic() {
    chat::test::FakeRedisClient redis;
    chat::RedisRateLimiter limiter(&redis, {}, []() { return 300; });
    std::atomic<int> allowed{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&]() {
            if (limiter.Allow("send", "7", 10, 60).allowed) {
                ++allowed;
            }
        });
    }
    for (std::thread &thread : threads) {
        thread.join();
    }
    assert(allowed == 10);
}

void TestRedisFailureFailsOpen() {
    chat::test::FakeRedisClient redis;
    redis.fail_commands = true;
    chat::RedisRateLimiter limiter(&redis, {});
    assert(limiter.Allow("register", "127.0.0.1", 1, 60).allowed);
}

}  // namespace

int main() {
    TestFixedWindowAndExpiry();
    TestConcurrentCountIsAtomic();
    TestRedisFailureFailsOpen();
    std::cout << "[PASS] redis rate limiter tests passed\n";
    return 0;
}
