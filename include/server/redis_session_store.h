#ifndef LINUX_SERVER_INCLUDE_SERVER_REDIS_SESSION_STORE_H_
#define LINUX_SERVER_INCLUDE_SERVER_REDIS_SESSION_STORE_H_

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "config/redis_config.h"
#include "model/connection_session.h"
#include "redis/redis_client.h"

namespace chat {

struct StoredSessionToken {
    UserId user_id = 0;
    std::string username;
    Timestamp issued_at = 0;
};

struct StoredUserPresence {
    std::string server_id;
    ConnectionId connection_id = 0;
    std::string token;
};

class IGlobalSessionStore {
   public:
    virtual ~IGlobalSessionStore() = default;

    virtual bool Bind(ConnectionId connection_id, const ConnectionSession &session, Timestamp issued_at) = 0;
    virtual bool Refresh(ConnectionId connection_id, const ConnectionSession &session) = 0;
    virtual bool ClearPresence(ConnectionId connection_id, const ConnectionSession &session) = 0;
    virtual bool RevokeToken(const std::string &token) = 0;
    virtual std::optional<StoredSessionToken> GetToken(const std::string &token) = 0;
    virtual std::optional<StoredUserPresence> GetPresence(UserId user_id) = 0;
};

// Redis 保存跨进程 token 和 presence，本地连接索引仍由 SessionManager 管理。
class RedisSessionStore : public IGlobalSessionStore {
   public:
    RedisSessionStore(IRedisClient *client, RedisConfig config);

    bool Bind(ConnectionId connection_id, const ConnectionSession &session, Timestamp issued_at) override;
    bool Refresh(ConnectionId connection_id, const ConnectionSession &session) override;
    bool ClearPresence(ConnectionId connection_id, const ConnectionSession &session) override;
    bool RevokeToken(const std::string &token) override;
    std::optional<StoredSessionToken> GetToken(const std::string &token) override;
    std::optional<StoredUserPresence> GetPresence(UserId user_id) override;

    std::string TokenKey(const std::string &token) const;
    std::string UserPresenceKey(UserId user_id) const;
    std::string ConnectionPresenceKey(const std::string &server_id, ConnectionId connection_id) const;

   private:
    bool IsValidSession(const ConnectionSession &session) const;
    std::optional<std::vector<std::string>> ReadHash(const std::string &key, const std::vector<std::string> &fields);

    IRedisClient *client_;
    RedisConfig config_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVER_REDIS_SESSION_STORE_H_
