#include "redis/redis_connection.h"

#include <cassert>
#include <iostream>

namespace {

void TestInvalidConfigFailsBeforeConnect() {
    chat::RedisConfig config;
    config.port = 0;
    chat::RedisConnection connection(config);
    const auto result = connection.Connect();
    assert(!result.ok());
    assert(result.error == chat::RedisError::kInvalidConfig);
}

void TestUnavailableAddressDoesNotCrash() {
    chat::RedisConfig config;
    config.host = "127.0.0.1";
    config.port = 1;
    config.connect_timeout_ms = 20;
    chat::RedisConnection connection(config);
    const auto result = connection.Connect();
    assert(!result.ok());
    assert(result.error == chat::RedisError::kConnectionUnavailable || result.error == chat::RedisError::kTimeout);
}

}  // namespace

int main() {
    TestInvalidConfigFailsBeforeConnect();
    TestUnavailableAddressDoesNotCrash();
    std::cout << "[PASS] redis connection tests passed\n";
    return 0;
}
