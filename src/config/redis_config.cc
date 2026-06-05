#include "config/redis_config.h"

#include <cstdlib>
#include <limits>

namespace chat {
namespace {

RedisConfigResult MakeError(RedisConfigError error, const std::string &message, const RedisConfig &config) {
    return {.success = false, .error = error, .message = message, .config = config};
}

RedisConfigResult MakeSuccess(const RedisConfig &config) {
    return {.success = true, .error = RedisConfigError::kNone, .message = "ok", .config = config};
}

bool ParseUnsigned(const char *value, unsigned long *parsed) {
    if (value == nullptr || *value == '\0' || *value == '-') {
        return false;
    }
    char *end = nullptr;
    *parsed = std::strtoul(value, &end, 10);
    return end != value && *end == '\0';
}

bool ParseBool(const char *value, bool *parsed) {
    if (value == nullptr || std::string(value) == "0") {
        *parsed = false;
        return true;
    }
    if (std::string(value) == "1") {
        *parsed = true;
        return true;
    }
    return false;
}

template <typename T>
bool ReadUnsignedEnv(const char *name, T *value) {
    const char *raw = std::getenv(name);
    if (raw == nullptr) {
        return true;
    }
    unsigned long parsed = 0;
    if (!ParseUnsigned(raw, &parsed) || parsed > static_cast<unsigned long>(std::numeric_limits<T>::max())) {
        return false;
    }
    *value = static_cast<T>(parsed);
    return true;
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
    if (config.key_prefix.empty()) {
        return MakeError(RedisConfigError::kInvalidKeyPrefix, "redis key prefix must not be empty", config);
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

RedisConfigResult LoadRedisConfigFromEnv() {
    RedisConfig config;
    if (const char *enabled = std::getenv("CHAT_REDIS_ENABLED"); !ParseBool(enabled, &config.enabled)) {
        return MakeError(RedisConfigError::kInvalidEnabled, "invalid CHAT_REDIS_ENABLED", config);
    }
    if (const char *value = std::getenv("CHAT_REDIS_HOST")) {
        config.host = value;
    }
    if (const char *value = std::getenv("CHAT_REDIS_PASSWORD")) {
        config.password = value;
    }
    if (const char *value = std::getenv("CHAT_REDIS_KEY_PREFIX")) {
        config.key_prefix = value;
    }
    if (const char *value = std::getenv("CHAT_SERVER_ID")) {
        config.server_id = value;
    }

    if (!ReadUnsignedEnv("CHAT_REDIS_PORT", &config.port)) {
        return MakeError(RedisConfigError::kInvalidPort, "invalid CHAT_REDIS_PORT", config);
    }
    if (!ReadUnsignedEnv("CHAT_REDIS_DB", &config.database)) {
        return MakeError(RedisConfigError::kInvalidDatabase, "invalid CHAT_REDIS_DB", config);
    }
    if (!ReadUnsignedEnv("CHAT_REDIS_POOL_SIZE", &config.pool_size)) {
        return MakeError(RedisConfigError::kInvalidPoolSize, "invalid CHAT_REDIS_POOL_SIZE", config);
    }
    if (!ReadUnsignedEnv("CHAT_REDIS_CONNECT_TIMEOUT_MS", &config.connect_timeout_ms) ||
        !ReadUnsignedEnv("CHAT_REDIS_COMMAND_TIMEOUT_MS", &config.command_timeout_ms)) {
        return MakeError(RedisConfigError::kInvalidTimeout, "invalid Redis timeout", config);
    }
    if (!ReadUnsignedEnv("CHAT_SESSION_TTL_SECONDS", &config.session_ttl_seconds) ||
        !ReadUnsignedEnv("CHAT_PRESENCE_TTL_SECONDS", &config.presence_ttl_seconds) ||
        !ReadUnsignedEnv("CHAT_USER_CACHE_TTL_SECONDS", &config.user_cache_ttl_seconds) ||
        !ReadUnsignedEnv("CHAT_USER_NOT_FOUND_TTL_SECONDS", &config.user_not_found_ttl_seconds) ||
        !ReadUnsignedEnv("CHAT_MESSAGE_DEDUP_TTL_SECONDS", &config.message_dedup_ttl_seconds)) {
        return MakeError(RedisConfigError::kInvalidTtl, "invalid Redis TTL", config);
    }
    if (!ReadUnsignedEnv("CHAT_LOGIN_RATE_LIMIT", &config.login_rate_limit) ||
        !ReadUnsignedEnv("CHAT_LOGIN_RATE_WINDOW_SECONDS", &config.login_rate_window_seconds) ||
        !ReadUnsignedEnv("CHAT_REGISTER_RATE_LIMIT", &config.register_rate_limit) ||
        !ReadUnsignedEnv("CHAT_REGISTER_RATE_WINDOW_SECONDS", &config.register_rate_window_seconds) ||
        !ReadUnsignedEnv("CHAT_SEND_RATE_LIMIT", &config.send_rate_limit) ||
        !ReadUnsignedEnv("CHAT_SEND_RATE_WINDOW_SECONDS", &config.send_rate_window_seconds)) {
        return MakeError(RedisConfigError::kInvalidRateLimit, "invalid Redis rate limit", config);
    }
    return ValidateRedisConfig(config);
}

}  // namespace chat
