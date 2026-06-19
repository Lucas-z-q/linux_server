#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_message_repository.h"
#include "handler/message_handler.h"
#include "server/session_manager.h"

namespace {

class EmptyUserRepository : public chat::IUserRepository {
   public:
    chat::FindUserResult findByUsername(const std::string&) override {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    chat::FindUserResult findById(chat::UserId) override { return {.status = chat::RepositoryStatus::kNotFound}; }
    chat::CreateUserResult createUser(const std::string&, const std::string&, const std::string&) override {
        return {.status = chat::RepositoryStatus::kInsertFailed};
    }
};

class InMemorySessionStore : public chat::IGlobalSessionStore {
   public:
    bool fail_reads = false;
    std::unordered_map<std::string, chat::StoredSessionToken> tokens;
    std::unordered_map<chat::UserId, chat::StoredUserPresence> presence;

    bool Bind(chat::ConnectionId connection_id, const chat::ConnectionSession& session,
              chat::Timestamp issued_at) override {
        const auto existing = presence.find(session.user_id);
        if (existing != presence.end() && existing->second.token != session.token) {
            tokens.erase(existing->second.token);
        }
        tokens[session.token] = {session.user_id, session.username, issued_at};
        presence[session.user_id] = {"test-server", connection_id, session.token};
        return true;
    }
    bool Refresh(chat::ConnectionId, const chat::ConnectionSession&) override { return true; }
    bool ClearPresence(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        const auto existing = presence.find(session.user_id);
        if (existing != presence.end() && existing->second.connection_id == connection_id &&
            existing->second.token == session.token) {
            presence.erase(existing);
        }
        return true;
    }
    bool RevokeSession(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        tokens.erase(session.token);
        ClearPresence(connection_id, session);
        return true;
    }
    bool RevokeToken(const std::string& token) override {
        tokens.erase(token);
        return true;
    }
    std::optional<chat::StoredSessionToken> GetToken(const std::string& token) override {
        if (fail_reads) {
            return std::nullopt;
        }
        const auto found = tokens.find(token);
        return found == tokens.end() ? std::nullopt : std::optional<chat::StoredSessionToken>(found->second);
    }
    std::optional<chat::StoredUserPresence> GetPresence(chat::UserId user_id) override {
        const auto found = presence.find(user_id);
        return found == presence.end() ? std::nullopt : std::optional<chat::StoredUserPresence>(found->second);
    }
};

nlohmann::json Parse(const HandleResult& result) { return nlohmann::json::parse(result.response); }

std::string Request(const std::string& type, int sequence, const std::string& token) {
    return nlohmann::json{{"msg_type", type}, {"seq", sequence}, {"token", token}, {"data", nlohmann::json::object()}}
        .dump();
}

void TestResumeBindMismatchAndLogoutReplay() {
    const std::string token(64, 'a');
    const std::string other_token(64, 'b');
    EmptyUserRepository users;
    FakeMessageRepository messages;
    chat::SessionManager sessions;
    InMemorySessionStore global;
    global.tokens[token] = {42, "alice", 123};
    chat::UserService user_service(users, sessions, &global);
    chat::ChatService chat_service(sessions, messages, users);
    chat::MessageHandler handler(user_service, chat_service);

    const HandleResult resumed = handler.handle(Request("resume_session", 1, token), 7);
    assert(resumed.session_action == SessionAction::BIND);
    assert(resumed.pending_session.user_id == 42);
    assert(resumed.pending_session.token == token);
    assert(Parse(resumed)["code"].get<int>() == static_cast<int>(chat::ErrorCode::OK));
    handler.applyBindSession(7, resumed.pending_session);

    const auto whoami = Parse(handler.handle(Request("whoami", 2, token), 7));
    assert(whoami["code"].get<int>() == static_cast<int>(chat::ErrorCode::OK));
    const auto mismatch = Parse(handler.handle(Request("whoami", 3, other_token), 7));
    assert(mismatch["code"].get<int>() == static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));

    const HandleResult logout = handler.handle(Request("logout", 4, token), 7);
    assert(logout.session_action == SessionAction::UNBIND);
    handler.applyUnbindSession(7);
    assert(global.tokens.count(token) == 0);

    const HandleResult replay = handler.handle(Request("resume_session", 5, token), 8);
    assert(replay.session_action == SessionAction::NONE);
    assert(Parse(replay)["code"].get<int>() == static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));
}

void TestUnknownMalformedAndStoreFailureFailClosed() {
    EmptyUserRepository users;
    FakeMessageRepository messages;
    chat::SessionManager sessions;
    InMemorySessionStore global;
    chat::UserService user_service(users, sessions, &global);
    chat::ChatService chat_service(sessions, messages, users);
    chat::MessageHandler handler(user_service, chat_service);

    assert(Parse(handler.handle(Request("resume_session", 1, std::string(64, 'c')), 9))["code"].get<int>() ==
           static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));
    assert(Parse(handler.handle(Request("resume_session", 2, "short"), 9))["code"].get<int>() ==
           static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));

    global.tokens[std::string(64, 'd')] = {42, "alice", 123};
    global.fail_reads = true;
    const HandleResult failed = handler.handle(Request("resume_session", 3, std::string(64, 'd')), 9);
    assert(failed.session_action == SessionAction::NONE);
    assert(Parse(failed)["code"].get<int>() == static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));
}

void TestRepeatedLoginRevokesOldConnectionToken() {
    const std::string old_token(64, 'e');
    const std::string new_token(64, 'f');
    EmptyUserRepository users;
    FakeMessageRepository messages;
    chat::SessionManager sessions;
    InMemorySessionStore global;
    chat::UserService user_service(users, sessions, &global);
    chat::ChatService chat_service(sessions, messages, users);
    chat::MessageHandler handler(user_service, chat_service);
    const chat::ConnectionSession old_session = {
        .authenticated = true,
        .user_id = 42,
        .username = "alice",
        .token = old_token,
    };
    const chat::ConnectionSession new_session = {
        .authenticated = true,
        .user_id = 42,
        .username = "alice",
        .token = new_token,
    };

    handler.applyBindSession(10, old_session);
    handler.applyBindSession(20, new_session);
    assert(!global.GetToken(old_token).has_value());
    assert(global.GetToken(new_token).has_value());

    const auto old_request = Parse(handler.handle(Request("whoami", 1, old_token), 10));
    assert(old_request["code"].get<int>() == static_cast<int>(chat::ErrorCode::INVALID_CREDENTIALS));
    const auto new_request = Parse(handler.handle(Request("whoami", 2, new_token), 20));
    assert(new_request["code"].get<int>() == static_cast<int>(chat::ErrorCode::OK));

    handler.onConnectionClosed(10);
    const auto presence = global.GetPresence(42);
    assert(presence.has_value());
    assert(presence->connection_id == 20);
    assert(presence->token == new_token);
}

}  // namespace

int main() {
    TestResumeBindMismatchAndLogoutReplay();
    TestUnknownMalformedAndStoreFailureFailClosed();
    TestRepeatedLoginRevokesOldConnectionToken();
    std::cout << "[PASS] token handler tests passed\n";
    return 0;
}
