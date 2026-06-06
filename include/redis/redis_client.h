#ifndef LINUX_SERVER_INCLUDE_REDIS_REDIS_CLIENT_H_
#define LINUX_SERVER_INCLUDE_REDIS_REDIS_CLIENT_H_

#include <string>
#include <vector>

#include "redis/redis_pool.h"

namespace chat {

class IRedisClient {
   public:
    virtual ~IRedisClient() = default;
    virtual RedisCommandResult Command(const std::vector<std::string> &args) = 0;
};

// 每条命令独占借用一个连接，命令失败时淘汰已断开的连接。
class RedisClient : public IRedisClient {
   public:
    explicit RedisClient(RedisPool *pool) : pool_(pool) {}
    RedisCommandResult Command(const std::vector<std::string> &args) override;

   private:
    RedisPool *pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_REDIS_REDIS_CLIENT_H_
