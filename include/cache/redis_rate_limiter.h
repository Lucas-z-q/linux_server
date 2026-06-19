#ifndef LINUX_SERVER_INCLUDE_CACHE_REDIS_RATE_LIMITER_H_
#define LINUX_SERVER_INCLUDE_CACHE_REDIS_RATE_LIMITER_H_

#include <cstdint>
#include <functional>
#include <string>

#include "config/redis_config.h"
#include "redis/redis_client.h"

namespace chat {

struct RateLimitResult {
    bool allowed = true;
    std::uint32_t retry_after_seconds = 0;
};

class IRateLimiter {
   public:
    virtual ~IRateLimiter() = default;
    virtual RateLimitResult Allow(const std::string &type, const std::string &identity, std::uint32_t limit,
                                  std::uint32_t window_seconds) = 0;
};

// Redis 失败时 fail-open，避免临时基础设施故障阻断核心业务。
class RedisRateLimiter : public IRateLimiter {
   public:
    using Clock = std::function<std::int64_t()>;

    RedisRateLimiter(IRedisClient *redis, RedisConfig config, Clock clock = {});
    RateLimitResult Allow(const std::string &type, const std::string &identity, std::uint32_t limit,
                          std::uint32_t window_seconds) override;

   private:
    IRedisClient *redis_;
    RedisConfig config_;
    Clock clock_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CACHE_REDIS_RATE_LIMITER_H_
