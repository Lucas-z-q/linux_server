#ifndef LINUX_SERVER_INCLUDE_CACHE_CACHED_USER_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_CACHE_CACHED_USER_REPOSITORY_H_

#include <optional>
#include <string>

#include "config/redis_config.h"
#include "db/user_repository.h"
#include "redis/redis_client.h"

namespace chat {

// 使用 Redis 装饰用户仓储。缓存故障时直接回源，不改变 Repository 语义。
class CachedUserRepository : public IUserRepository {
   public:
    CachedUserRepository(IUserRepository *source, IRedisClient *redis, RedisConfig config);

    FindUserResult findByUsername(const std::string &username) override;
    FindUserResult findById(UserId user_id) override;
    CreateUserResult createUser(const std::string &username, const std::string &password_hash,
                                const std::string &nickname) override;
    RepositoryStatus updatePasswordHash(UserId user_id, const std::string &password_hash) override;

   private:
    std::string UserKey(UserId user_id) const;
    std::string UsernameKey(const std::string &username) const;
    void CacheNotFound(const std::string &key);

    IUserRepository *source_;
    IRedisClient *redis_;
    RedisConfig config_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CACHE_CACHED_USER_REPOSITORY_H_
