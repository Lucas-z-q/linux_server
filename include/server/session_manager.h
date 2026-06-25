#ifndef LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_
#define LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_

// Declares a lightweight in-memory session manager.
//
// This component only owns process-local connection state. Global token and
// presence state belong to RedisSessionStore.
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "model/connection_session.h"

namespace chat {
class ISessionManager {
   public:
    virtual ~ISessionManager() = default;

    // Binds one authenticated session to a connection.
    virtual bool BindSession(ConnectionId connection_id, const ConnectionSession &session) = 0;

    // Returns the active connection ID for the specified user when present.
    virtual std::optional<ConnectionId> GetConnectionId(UserId user_id) = 0;

    virtual std::vector<ConnectionId> GetConnectionIds(UserId user_id) {
        const std::optional<ConnectionId> connection_id = GetConnectionId(user_id);
        return connection_id.has_value() ? std::vector<ConnectionId>{*connection_id} : std::vector<ConnectionId>{};
    }

    // Returns the session when the given connection currently has one.
    virtual std::optional<ConnectionSession> GetSession(ConnectionId connection_id) = 0;

    // Removes the session bound to the specified connection.
    virtual void ClearSession(ConnectionId connection_id) = 0;
};

// Manages the relationship between authenticated users and active
// connections.
class SessionManager : public ISessionManager {
   public:
    SessionManager() = default;
    ~SessionManager() override = default;

    bool BindSession(ConnectionId connection_id, const ConnectionSession &session) override;

    std::optional<ConnectionId> GetConnectionId(UserId user_id) override;

    std::vector<ConnectionId> GetConnectionIds(UserId user_id) override;

    std::optional<ConnectionSession> GetSession(ConnectionId connection_id) override;

    void ClearSession(ConnectionId connection_id) override;

   private:
    std::mutex mutex_;
    std::unordered_map<UserId, std::vector<ConnectionId>> user_to_connections_;
    std::unordered_map<ConnectionId, ConnectionSession> connection_to_session_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_
