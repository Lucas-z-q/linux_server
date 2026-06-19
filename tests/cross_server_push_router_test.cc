#include <cassert>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

#include "fake_message_repository.h"
#include "fake_user_repository.h"
#include "service/chat_service.h"

namespace {

class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;
    std::unordered_map<chat::UserId, chat::ConnectionId> connections;
    std::unordered_map<chat::UserId, std::vector<chat::ConnectionId>> connection_lists;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        sessions[connection_id] = session;
        connections[session.user_id] = connection_id;
        connection_lists[session.user_id].push_back(connection_id);
        return true;
    }
    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        const auto it = connections.find(user_id);
        return it == connections.end() ? std::nullopt : std::optional<chat::ConnectionId>(it->second);
    }
    std::vector<chat::ConnectionId> GetConnectionIds(chat::UserId user_id) override {
        const auto it = connection_lists.find(user_id);
        return it == connection_lists.end() ? std::vector<chat::ConnectionId>() : it->second;
    }
    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        const auto it = sessions.find(connection_id);
        return it == sessions.end() ? std::nullopt : std::optional<chat::ConnectionSession>(it->second);
    }
    void ClearSession(chat::ConnectionId connection_id) override { sessions.erase(connection_id); }
};

class FakeGlobalSessionStore : public chat::IGlobalSessionStore {
   public:
    std::optional<chat::StoredUserPresence> presence;

    bool Bind(chat::ConnectionId, const chat::ConnectionSession &, chat::Timestamp) override { return true; }
    bool Refresh(chat::ConnectionId, const chat::ConnectionSession &) override { return true; }
    bool ClearPresence(chat::ConnectionId, const chat::ConnectionSession &) override { return true; }
    bool RevokeToken(const std::string &) override { return true; }
    std::optional<chat::StoredSessionToken> GetToken(const std::string &) override { return std::nullopt; }
    std::optional<chat::StoredUserPresence> GetPresence(chat::UserId) override { return presence; }
};

class FakePublisher : public chat::IRemotePushPublisher {
   public:
    bool result = true;
    int calls = 0;
    std::string target_server;
    chat::RemotePushEvent event;

    bool Publish(const std::string &server_id, const chat::RemotePushEvent &value) override {
        ++calls;
        target_server = server_id;
        event = value;
        return result;
    }
};

void BindUser(FakeSessionManager *sessions, chat::ConnectionId connection_id, chat::UserId user_id) {
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = user_id;
    session.username = "user" + std::to_string(user_id);
    sessions->BindSession(connection_id, session);
}

chat::SendMessageRequest MakeRequest() { return {.client_msg_id = "client-1", .to_user_id = 2, .content = "hello"}; }

void TestRemotePresenceRoutesToOtherServer() {
    FakeSessionManager sessions;
    BindUser(&sessions, 10, 1);
    FakeMessageRepository messages;
    FakeUserRepository users;
    FakeGlobalSessionStore global;
    global.presence = chat::StoredUserPresence{"server-b", 42, "token"};
    FakePublisher publisher;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::ChatService service(sessions, messages, users, nullptr, nullptr, config, &global, &publisher);

    const chat::SendMessageResult result = service.sendMessage(10, MakeRequest());

    assert(result.code == chat::ErrorCode::OK);
    assert(result.to_conn_id == 0);
    assert(result.remote_server_id == "server-b");
    assert(result.remote_conn_id == 42);
    assert(service.publishRemotePush(result, "payload"));
    assert(publisher.calls == 1);
    assert(publisher.target_server == "server-b");
    assert(publisher.event.target_connection_id == 42);
}

void TestLocalPresenceRequiresMatchingConnection() {
    FakeSessionManager sessions;
    BindUser(&sessions, 10, 1);
    BindUser(&sessions, 20, 2);
    FakeMessageRepository messages;
    FakeUserRepository users;
    FakeGlobalSessionStore global;
    global.presence = chat::StoredUserPresence{"server-a", 20, "token"};
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::ChatService service(sessions, messages, users, nullptr, nullptr, config, &global);

    assert(service.sendMessage(10, MakeRequest()).to_conn_id == 20);

    global.presence = chat::StoredUserPresence{"server-a", 21, "new-token"};
    const chat::SendMessageRequest rebound_request{
        .client_msg_id = "client-2", .to_user_id = 2, .content = "hello again"};
    const auto rebound = service.sendMessage(10, rebound_request);
    assert(rebound.to_conn_id == 0);
    assert(rebound.remote_server_id.empty());
}

void TestLocalPresenceRoutesAllLocalConnections() {
    FakeSessionManager sessions;
    BindUser(&sessions, 10, 1);
    BindUser(&sessions, 20, 2);
    BindUser(&sessions, 21, 2);
    FakeMessageRepository messages;
    FakeUserRepository users;
    FakeGlobalSessionStore global;
    global.presence = chat::StoredUserPresence{"server-a", 21, "token"};
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::ChatService service(sessions, messages, users, nullptr, nullptr, config, &global);

    const auto result = service.sendMessage(10, MakeRequest());

    assert(result.remote_server_id.empty());
    assert(result.to_conn_ids.size() == 2);
    assert(result.to_conn_ids[0] == 20);
    assert(result.to_conn_ids[1] == 21);
}

void TestStreamFailureKeepsStoredSendSuccess() {
    FakeSessionManager sessions;
    BindUser(&sessions, 10, 1);
    FakeMessageRepository messages;
    FakeUserRepository users;
    FakeGlobalSessionStore global;
    global.presence = chat::StoredUserPresence{"server-b", 42, "token"};
    FakePublisher publisher;
    publisher.result = false;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::ChatService service(sessions, messages, users, nullptr, nullptr, config, &global, &publisher);

    const auto result = service.sendMessage(10, MakeRequest());
    assert(result.code == chat::ErrorCode::OK);
    assert(!service.publishRemotePush(result, "payload"));
    assert(messages.created_messages[0].status == chat::MessageStatus::kStored);
}

}  // namespace

int main() {
    TestRemotePresenceRoutesToOtherServer();
    TestLocalPresenceRequiresMatchingConnection();
    TestLocalPresenceRoutesAllLocalConnections();
    TestStreamFailureKeepsStoredSendSuccess();
    std::cout << "[PASS] cross server push router tests passed\n";
    return 0;
}
