#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_message_repository.h"
#include "fake_user_repository.h"
#include "service/chat_service.h"

namespace {

class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        sessions[connection_id] = session;
        return true;
    }
    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        (void)user_id;
        return std::nullopt;
    }
    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        const auto it = sessions.find(connection_id);
        return it == sessions.end() ? std::nullopt : std::optional<chat::ConnectionSession>(it->second);
    }
    void ClearSession(chat::ConnectionId connection_id) override { sessions.erase(connection_id); }
};

class FakeRateLimiter : public chat::IRateLimiter {
   public:
    chat::RateLimitResult result;
    chat::RateLimitResult Allow(const std::string &type, const std::string &identity, std::uint32_t limit,
                                std::uint32_t window_seconds) override {
        (void)type;
        (void)identity;
        (void)limit;
        (void)window_seconds;
        return result;
    }
};

class FakeDedupCache : public chat::IMessageDedupCache {
   public:
    std::optional<std::string> value;
    int save_calls = 0;
    int remove_calls = 0;

    std::optional<std::string> Lookup(chat::UserId from_user_id, const std::string &client_msg_id) override {
        (void)from_user_id;
        (void)client_msg_id;
        return value;
    }
    void Save(chat::UserId from_user_id, const std::string &client_msg_id, const std::string &message_id) override {
        (void)from_user_id;
        (void)client_msg_id;
        value = message_id;
        ++save_calls;
    }
    void Remove(chat::UserId from_user_id, const std::string &client_msg_id) override {
        (void)from_user_id;
        (void)client_msg_id;
        value.reset();
        ++remove_calls;
    }
};

void BindSender(FakeSessionManager *sessions) {
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 1;
    session.username = "alice";
    sessions->BindSession(100, session);
}

chat::SendMessageRequest MakeRequest() {
    chat::SendMessageRequest request;
    request.client_msg_id = "client-1";
    request.to_user_id = 2;
    request.content = "hello";
    return request;
}

chat::MessageRecord ExistingMessage(const std::string &content) {
    chat::MessageRecord message;
    message.id = "message-1";
    message.conversation_id = "conv_1_2";
    message.client_msg_id = "client-1";
    message.from_user_id = 1;
    message.to_user_id = 2;
    message.content = content;
    message.status = chat::MessageStatus::kStored;
    return message;
}

void TestSendRateLimitStopsBeforeStorage() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    FakeRateLimiter limiter;
    limiter.result = {.allowed = false, .retry_after_seconds = 11};
    chat::ChatService service(sessions, messages, users, &limiter);

    const auto result = service.sendMessage(100, MakeRequest());

    assert(result.code == chat::ErrorCode::RATE_LIMITED);
    assert(result.retry_after_seconds == 11);
    assert(messages.create_message_calls == 0);
}

void TestDedupHitStillVerifiesMysqlRecord() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    messages.created_messages.push_back(ExistingMessage("hello"));
    FakeUserRepository users;
    FakeDedupCache dedup;
    dedup.value = "message-1";
    chat::ChatService service(sessions, messages, users, nullptr, &dedup);

    const auto result = service.sendMessage(100, MakeRequest());

    assert(result.code == chat::ErrorCode::OK);
    assert(messages.find_by_client_msg_id_calls == 1);
    assert(messages.create_message_calls == 0);
    assert(dedup.save_calls == 1);
}

void TestDedupConflictDoesNotRefreshCache() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    messages.created_messages.push_back(ExistingMessage("different"));
    FakeUserRepository users;
    FakeDedupCache dedup;
    dedup.value = "message-1";
    chat::ChatService service(sessions, messages, users, nullptr, &dedup);

    const auto result = service.sendMessage(100, MakeRequest());

    assert(result.code == chat::ErrorCode::IDEMPOTENCY_CONFLICT);
    assert(messages.find_by_client_msg_id_calls == 1);
    assert(messages.create_message_calls == 0);
    assert(dedup.save_calls == 0);
}

}  // namespace

int main() {
    TestSendRateLimitStopsBeforeStorage();
    TestDedupHitStillVerifiesMysqlRecord();
    TestDedupConflictDoesNotRefreshCache();
    std::cout << "[PASS] chat service redis tests passed\n";
    return 0;
}
