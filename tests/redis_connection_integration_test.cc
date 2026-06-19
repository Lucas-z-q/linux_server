#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "redis/redis_connection.h"

namespace {

bool IsEnabled() {
    const char* enabled = std::getenv("CHAT_REDIS_TEST_ENABLED");
    return enabled != nullptr && std::string(enabled) == "1";
}

chat::RedisConfig LoadIntegrationConfig() {
    chat::RedisConfig config;
    if (const char* host = std::getenv("CHAT_REDIS_TEST_HOST")) {
        config.host = host;
    }
    if (const char* port = std::getenv("CHAT_REDIS_TEST_PORT")) {
        config.port = static_cast<std::uint16_t>(std::stoi(port));
    }
    if (const char* password = std::getenv("CHAT_REDIS_TEST_PASSWORD")) {
        config.password = password;
    }
    if (const char* database = std::getenv("CHAT_REDIS_TEST_DB")) {
        config.database = std::stoi(database);
    }
    config.pool_size = 1;
    config.connect_timeout_ms = 1000;
    config.command_timeout_ms = 1000;
    return config;
}

}  // namespace

int main() {
    if (!IsEnabled()) {
        std::cout << "[SKIP] redis integration test disabled\n";
        return 0;
    }

    chat::RedisConnection connection(LoadIntegrationConfig());
    const chat::RedisConnectionResult init = connection.Connect();
    assert(init.ok());

    const chat::RedisCommandResult ping = connection.Ping();
    assert(ping.ok());
    assert(ping.reply.string_value == "PONG");

    std::cout << "[PASS] redis integration ping passed\n";
    return 0;
}
