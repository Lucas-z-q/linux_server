#ifndef LINUX_SERVER_TESTS_IN_MEMORY_SESSION_STORE_H_
#define LINUX_SERVER_TESTS_IN_MEMORY_SESSION_STORE_H_

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "server/redis_session_store.h"

class InMemorySessionStore : public chat::IGlobalSessionStore {
   public:
    bool Bind(chat::ConnectionId connection_id, const chat::ConnectionSession& session,
              chat::Timestamp issued_at) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = presence_.find(session.user_id);
        if (existing != presence_.end() && existing->second.token != session.token) {
            tokens_.erase(existing->second.token);
        }
        tokens_[session.token] = {session.user_id, session.username, issued_at};
        presence_[session.user_id] = {"test-server", connection_id, session.token};
        return true;
    }

    bool Refresh(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = presence_.find(session.user_id);
        return existing != presence_.end() && existing->second.connection_id == connection_id &&
               existing->second.token == session.token;
    }

    bool ClearPresence(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = presence_.find(session.user_id);
        if (existing != presence_.end() && existing->second.connection_id == connection_id &&
            existing->second.token == session.token) {
            presence_.erase(existing);
        }
        return true;
    }

    bool RevokeSession(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tokens_.erase(session.token);
        const auto existing = presence_.find(session.user_id);
        if (existing != presence_.end() && existing->second.connection_id == connection_id &&
            existing->second.token == session.token) {
            presence_.erase(existing);
        }
        return true;
    }

    bool RevokeToken(const std::string& token) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tokens_.erase(token);
        return true;
    }

    std::optional<chat::StoredSessionToken> GetToken(const std::string& token) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = tokens_.find(token);
        if (existing == tokens_.end()) {
            return std::nullopt;
        }
        return existing->second;
    }

    std::optional<chat::StoredUserPresence> GetPresence(chat::UserId user_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = presence_.find(user_id);
        if (existing == presence_.end()) {
            return std::nullopt;
        }
        return existing->second;
    }

   private:
    std::mutex mutex_;
    std::unordered_map<std::string, chat::StoredSessionToken> tokens_;
    std::unordered_map<chat::UserId, chat::StoredUserPresence> presence_;
};

#endif  // LINUX_SERVER_TESTS_IN_MEMORY_SESSION_STORE_H_
