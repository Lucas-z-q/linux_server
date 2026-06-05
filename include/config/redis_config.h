#ifndef LINUX_SERVER_INCLUDE_CONFIG_REDIS_CONFIG_H_
#define LINUX_SERVER_INCLUDE_CONFIG_REDIS_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace chat {

struct RedisConfig {
    bool enabled = false;
    std::string host = "127.0.0.1";
    std::uint16_t port = 6379;
    std::string password;
    int database = 0;
    std::size_t pool_size = 4;
    std::uint32_t connect_timeout_ms = 500;
    std::uint32_t command_timeout_ms = 500;
    std::string key_prefix = "chat";
    std::string server_id = "server-1";
    std::uint32_t session_ttl_seconds = 604800;
    std::uint32_t presence_ttl_seconds = 90;
};

enum class RedisConfigError {
    kNone = 0,
    kInvalidEnabled,
    kInvalidPort,
    kInvalidDatabase,
    kInvalidPoolSize,
    kInvalidTimeout,
    kInvalidKeyPrefix,
    kInvalidServerId,
    kInvalidTtl,
};

struct RedisConfigResult {
    bool success = false;
    RedisConfigError error = RedisConfigError::kNone;
    std::string message;
    RedisConfig config;

    bool ok() const noexcept { return success; }
};

RedisConfigResult LoadRedisConfigFromEnv();
RedisConfigResult ValidateRedisConfig(const RedisConfig &config);

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_REDIS_CONFIG_H_
