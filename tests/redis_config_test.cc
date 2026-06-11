#include "config/redis_config.h"

#include <cassert>
#include <iostream>

namespace {

void TestDefaults() {
    const chat::RedisConfig config;
    const chat::RedisConfigResult result = chat::ValidateRedisConfig(config);
    assert(result.ok());
    assert(!config.enabled);
    assert(config.connect_timeout_ms == 500);
    assert(config.command_timeout_ms == 500);
    assert(config.key_prefix == "chat");
    assert(config.server_id == "server-1");
    assert(config.session_ttl_seconds == 604800);
    assert(config.presence_ttl_seconds == 120);
}

void TestInvalidValues() {
    {
        chat::RedisConfig config;
        config.pool_size = 0;
        assert(!chat::ValidateRedisConfig(config).ok());
    }
    {
        chat::RedisConfig config;
        config.key_prefix = "chat:";
        assert(!chat::ValidateRedisConfig(config).ok());
    }
    {
        chat::RedisConfig config;
        config.server_id = "bad:id";
        assert(!chat::ValidateRedisConfig(config).ok());
    }
    {
        chat::RedisConfig config;
        config.command_timeout_ms = 0;
        assert(!chat::ValidateRedisConfig(config).ok());
    }
}

}  // namespace

int main() {
    TestDefaults();
    TestInvalidValues();
    std::cout << "[PASS] redis config tests passed\n";
    return 0;
}
