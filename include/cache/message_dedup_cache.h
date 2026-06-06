#ifndef LINUX_SERVER_INCLUDE_CACHE_MESSAGE_DEDUP_CACHE_H_
#define LINUX_SERVER_INCLUDE_CACHE_MESSAGE_DEDUP_CACHE_H_

#include <optional>
#include <string>

#include "common/types.h"
#include "config/redis_config.h"
#include "redis/redis_client.h"

namespace chat {

class IMessageDedupCache {
   public:
    virtual ~IMessageDedupCache() = default;
    virtual std::optional<std::string> Lookup(UserId from_user_id, const std::string &client_msg_id) = 0;
    virtual void Save(UserId from_user_id, const std::string &client_msg_id, const std::string &message_id) = 0;
    virtual void Remove(UserId from_user_id, const std::string &client_msg_id) = 0;
};

class MessageDedupCache : public IMessageDedupCache {
   public:
    MessageDedupCache(IRedisClient *redis, RedisConfig config);

    std::optional<std::string> Lookup(UserId from_user_id, const std::string &client_msg_id) override;
    void Save(UserId from_user_id, const std::string &client_msg_id, const std::string &message_id) override;
    void Remove(UserId from_user_id, const std::string &client_msg_id) override;

   private:
    std::string Key(UserId from_user_id, const std::string &client_msg_id) const;

    IRedisClient *redis_;
    RedisConfig config_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CACHE_MESSAGE_DEDUP_CACHE_H_
