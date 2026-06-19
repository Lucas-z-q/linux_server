#include "server/session_manager.h"

#include <algorithm>

namespace chat {

bool SessionManager::BindSession(ConnectionId connection_id, const ConnectionSession &session) {
    if (!session.authenticated || session.user_id <= 0 || session.username.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing_conn_it = connection_to_session_.find(connection_id);
    if (existing_conn_it != connection_to_session_.end()) {
        auto &old_connections = user_to_connections_[existing_conn_it->second.user_id];
        old_connections.erase(std::remove(old_connections.begin(), old_connections.end(), connection_id),
                              old_connections.end());
        if (old_connections.empty()) {
            user_to_connections_.erase(existing_conn_it->second.user_id);
        }
    }

    auto &connections = user_to_connections_[session.user_id];
    if (std::find(connections.begin(), connections.end(), connection_id) == connections.end()) {
        connections.push_back(connection_id);
    }
    connection_to_session_[connection_id] = session;
    return true;
}

std::optional<ConnectionId> SessionManager::GetConnectionId(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = user_to_connections_.find(user_id);
    if (it == user_to_connections_.end() || it->second.empty()) {
        return std::nullopt;
    }

    return it->second.back();
}

std::vector<ConnectionId> SessionManager::GetConnectionIds(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = user_to_connections_.find(user_id);
    if (it == user_to_connections_.end()) {
        return {};
    }
    return it->second;
}

std::optional<ConnectionSession> SessionManager::GetSession(ConnectionId connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = connection_to_session_.find(connection_id);
    if (it == connection_to_session_.end()) {
        return std::nullopt;
    }

    return it->second;
}

void SessionManager::ClearSession(ConnectionId connection_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = connection_to_session_.find(connection_id);
    if (it == connection_to_session_.end()) {
        return;
    }

    auto &connections = user_to_connections_[it->second.user_id];
    connections.erase(std::remove(connections.begin(), connections.end(), connection_id), connections.end());
    if (connections.empty()) {
        user_to_connections_.erase(it->second.user_id);
    }
    connection_to_session_.erase(it);
}

}  // namespace chat
