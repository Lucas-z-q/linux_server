#include "server/session_manager.h"

namespace chat {

bool SessionManager::BindSession(ConnectionId connection_id, const ConnectionSession &session) {
    if (!session.authenticated || session.user_id <= 0 || session.username.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    const auto existing_user_it = user_to_connection_.find(session.user_id);
    if (existing_user_it != user_to_connection_.end()) {
        connection_to_session_.erase(existing_user_it->second);
    }

    const auto existing_conn_it = connection_to_session_.find(connection_id);
    if (existing_conn_it != connection_to_session_.end() && existing_conn_it->second.user_id != session.user_id) {
        user_to_connection_.erase(existing_conn_it->second.user_id);
    }

    user_to_connection_[session.user_id] = connection_id;
    connection_to_session_[connection_id] = session;
    return true;
}

std::optional<ConnectionId> SessionManager::GetConnectionId(UserId user_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto it = user_to_connection_.find(user_id);
    if (it == user_to_connection_.end()) {
        return std::nullopt;
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

    user_to_connection_.erase(it->second.user_id);
    connection_to_session_.erase(it);
}

}  // namespace chat
