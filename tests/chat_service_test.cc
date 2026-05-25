#include "service/chat_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;
    std::unordered_map<chat::UserId, chat::ConnectionId> user_conns;

    bool BindSession(chat::ConnectionId connection_id,
                     const chat::ConnectionSession& session) override {
        sessions[connection_id] = session;
        if (session.authenticated) {
            user_conns[session.user_id] = connection_id;
        }
        return true;
    }

    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        auto it = user_conns.find(user_id);
        if (it != user_conns.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        auto it = sessions.find(connection_id);
        if (it != sessions.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    void ClearSession(chat::ConnectionId connection_id) override {
        auto it = sessions.find(connection_id);
        if (it != sessions.end()) {
            user_conns.erase(it->second.user_id);
            sessions.erase(it);
        }
    }
};

void TestSendMessageNotLoggedIn() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    chat::SendMessageRequest req;
    req.receiver_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::NOT_LOGGED_IN);
}

void TestSendMessageToSelf() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    // Setup sender session
    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.receiver_id = 1;  // Sending to self
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::CANNOT_SEND_TO_SELF);
}

void TestSendMessageTargetNotOnline() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    // Setup sender session
    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.receiver_id = 2;  // Target user offline
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::USER_NOT_ONLINE);
}

void TestSendMessageSuccess() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    // Setup sender session
    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    // Setup receiver session
    chat::ConnectionSession receiver_session;
    receiver_session.authenticated = true;
    receiver_session.user_id = 2;
    receiver_session.username = "receiver";
    session_manager.BindSession(200, receiver_session);

    chat::SendMessageRequest req;
    req.receiver_id = 2;
    req.content = "hello world";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.from_user_id == 1);
    assert(result.from_username == "sender");
    assert(result.to_user_id == 2);
    assert(result.to_conn_id == 200);
    assert(result.content == "hello world");
    assert(result.server_time > 0);
}

void TestSendMessageEmptyContent() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    chat::SendMessageRequest req;
    req.receiver_id = 2;
    req.content = "";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
}

void TestSendMessageTooLongContent() {
    FakeSessionManager session_manager;
    chat::ChatService chat_service(session_manager);

    chat::SendMessageRequest req;
    req.receiver_id = 2;
    req.content = std::string(4097, 'x');

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::MESSAGE_TOO_LONG);
}

}  // namespace

int main() {
    TestSendMessageNotLoggedIn();
    TestSendMessageToSelf();
    TestSendMessageTargetNotOnline();
    TestSendMessageSuccess();
    TestSendMessageEmptyContent();
    TestSendMessageTooLongContent();
    std::cout << "[PASS] chat service tests passed\n";
    return 0;
}
