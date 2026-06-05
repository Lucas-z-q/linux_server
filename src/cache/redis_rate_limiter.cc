#include "cache/redis_rate_limiter.h"

#include <chrono>
#include <utility>

namespace chat {
namespace {

constexpr char kRateLimitScript[] = R"(
local count = redis.call('INCR', KEYS[1])
if count == 1 then
  redis.call('EXPIRE', KEYS[1], ARGV[1])
end
local ttl = redis.call('TTL', KEYS[1])
return {count, ttl}
)";

std::int64_t SystemTimeSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

RedisRateLimiter::RedisRateLimiter(IRedisClient *redis, RedisConfig config, Clock clock)
    : redis_(redis), config_(std::move(config)), clock_(clock ? std::move(clock) : SystemTimeSeconds) {}

RateLimitResult RedisRateLimiter::Allow(const std::string &type, const std::string &identity, std::uint32_t limit,
                                        std::uint32_t window_seconds) {
    if (redis_ == nullptr || type.empty() || identity.empty() || limit == 0 || window_seconds == 0) {
        return {};
    }
    const std::int64_t bucket = clock_() / window_seconds;
    const std::string key = config_.key_prefix + ":ratelimit:" + type + ":" + identity + ":" + std::to_string(bucket);
    const RedisCommandResult result =
        redis_->Command({"EVAL", kRateLimitScript, "1", key, std::to_string(window_seconds)});
    if (!result.ok() || result.reply.type != RedisReplyType::kArray || result.reply.elements.size() != 2 ||
        result.reply.elements[0].type != RedisReplyType::kInteger ||
        result.reply.elements[1].type != RedisReplyType::kInteger) {
        return {};
    }
    const long long count = result.reply.elements[0].integer_value;
    const long long ttl = result.reply.elements[1].integer_value;
    return {.allowed = count <= limit,
            .retry_after_seconds = count <= limit ? 0 : static_cast<std::uint32_t>(ttl > 0 ? ttl : window_seconds)};
}

}  // namespace chat
