#include "cache/message_dedup_cache.h"

#include <cassert>
#include <iostream>

#include "fake_redis_client.h"

int main() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.message_dedup_ttl_seconds = 123;
    chat::MessageDedupCache cache(&redis, config);

    assert(!cache.Lookup(7, "client-1"));
    cache.Save(7, "client-1", "message-1");
    assert(cache.Lookup(7, "client-1") == "message-1");
    assert(redis.ttls["chat:dedup:msg:7:client-1"] == 123);

    cache.Remove(7, "client-1");
    assert(!cache.Lookup(7, "client-1"));

    redis.fail_commands = true;
    assert(!cache.Lookup(7, "client-2"));
    cache.Save(7, "client-2", "message-2");
    cache.Remove(7, "client-2");

    std::cout << "[PASS] message dedup cache tests passed\n";
    return 0;
}
