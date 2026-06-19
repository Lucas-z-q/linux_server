#include "config/redis_config.h"

namespace chat {
namespace {

RedisConfigResult MakeError(RedisConfigError error, const std::string &message, const RedisConfig &config) {
    return {.success = false, .error = error, .message = message, .config = config};
}

RedisConfigResult MakeSuccess(const RedisConfig &config) {
    return {.success = true, .error = RedisConfigError::kNone, .message = "ok", .config = config};
}

}  // namespace

RedisConfigResult ValidateRedisConfig(const RedisConfig &config) {
    if (config.port == 0) {
        return MakeError(RedisConfigError::kInvalidPort, "redis port must be positive", config);
    }
    if (config.database < 0) {
        return MakeError(RedisConfigError::kInvalidDatabase, "redis database must be non-negative", config);
    }
    if (config.pool_size == 0) {
        return MakeError(RedisConfigError::kInvalidPoolSize, "redis pool size must be positive", config);
    }
    if (config.connect_timeout_ms == 0 || config.command_timeout_ms == 0) {
        return MakeError(RedisConfigError::kInvalidTimeout, "redis timeouts must be positive", config);
    }
    if (config.key_prefix.empty() || config.key_prefix.back() == ':') {
        return MakeError(RedisConfigError::kInvalidKeyPrefix, "redis key prefix must not be empty or end with a colon",
                         config);
    }
    if (config.server_id.empty() || config.server_id.find(':') != std::string::npos) {
        return MakeError(RedisConfigError::kInvalidServerId, "server id must be non-empty and contain no colon",
                         config);
    }
    if (config.session_ttl_seconds == 0 || config.presence_ttl_seconds == 0 || config.user_cache_ttl_seconds == 0 ||
        config.user_not_found_ttl_seconds == 0 || config.message_dedup_ttl_seconds == 0) {
        return MakeError(RedisConfigError::kInvalidTtl, "redis TTL values must be positive", config);
    }
    if (config.login_rate_limit == 0 || config.login_rate_window_seconds == 0 || config.register_rate_limit == 0 ||
        config.register_rate_window_seconds == 0 || config.send_rate_limit == 0 ||
        config.send_rate_window_seconds == 0) {
        return MakeError(RedisConfigError::kInvalidRateLimit, "redis rate limits must be positive", config);
    }
    return MakeSuccess(config);
}

}  // namespace chat
