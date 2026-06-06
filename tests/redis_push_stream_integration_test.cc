#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>

#include "redis/redis_client.h"
#include "redis/redis_pool.h"
#include "stream/redis_push_stream.h"

namespace {

bool IsEnabled() {
    const char *enabled = std::getenv("CHAT_REDIS_TEST_ENABLED");
    return enabled != nullptr && std::string(enabled) == "1";
}

chat::RedisConfig LoadConfig() {
    chat::RedisConfig config;
    config.enabled = true;
    if (const char *host = std::getenv("CHAT_REDIS_TEST_HOST")) {
        config.host = host;
    }
    if (const char *port = std::getenv("CHAT_REDIS_TEST_PORT")) {
        config.port = static_cast<std::uint16_t>(std::stoi(port));
    }
    if (const char *password = std::getenv("CHAT_REDIS_TEST_PASSWORD")) {
        config.password = password;
    }
    if (const char *database = std::getenv("CHAT_REDIS_TEST_DB")) {
        config.database = std::stoi(database);
    }
    config.pool_size = 2;
    config.command_timeout_ms = 1000;
    config.server_id = "integration-" + std::to_string(getpid());
    config.key_prefix = "chat:test:" + std::to_string(getpid());
    return config;
}

}  // namespace

int main() {
    if (!IsEnabled()) {
        std::cout << "[SKIP] redis push stream integration test disabled\n";
        return 0;
    }

    chat::RedisConfig config = LoadConfig();
    chat::RedisPool pool(config);
    assert(pool.Init().ok());
    chat::RedisClient client(&pool);
    chat::RedisPushStream stream(&client, config);
    assert(stream.Initialize());

    int delivered = 0;
    int marked = 0;
    stream.SetDeliveryCallback([&](const chat::RemotePushEvent &) {
        ++delivered;
        return chat::RemoteDeliveryOutcome::kDelivered;
    });
    stream.SetMarkDeliveredCallback([&](chat::UserId, const std::string &) {
        ++marked;
        return true;
    });

    chat::RemotePushEvent event{"integration-message",           "integration-message", 1, 2, 42,
                                R"({"msg_type":"message_push"})"};
    assert(stream.Publish(config.server_id, event));
    assert(stream.PollOnce());
    assert(delivered == 1);
    assert(marked == 1);

    client.Command({"DEL", stream.PushStreamKey(config.server_id), stream.RetryStreamKey(config.server_id),
                    stream.DeadLetterStreamKey(config.server_id),
                    config.key_prefix + ":push:dedup:" + config.server_id + ":integration-message"});
    std::cout << "[PASS] redis push stream integration test passed\n";
    return 0;
}
