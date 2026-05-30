#include "service/chat_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_message_repository.h"
#include "fake_user_repository.h"

namespace {

class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;
    std::unordered_map<chat::UserId, chat::ConnectionId> user_conns;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
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
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_not_logged_in";
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::NOT_LOGGED_IN);
}

void TestSendMessageToSelf() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    // Setup sender session
    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_to_self";
    req.to_user_id = 1;  // Sending to self
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::CANNOT_SEND_TO_SELF);
}

void TestSendMessageTargetNotExists() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_target_missing";
    req.to_user_id = 999;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::USER_NOT_FOUND);
    assert(message_repo.created_messages.empty());
}

void TestSendMessageTargetNotOnline() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    // Setup sender session
    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_offline";
    req.to_user_id = 2;  // Target user offline
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.to_conn_id == 0);
    assert(result.status == chat::ToProtocolMessageStatus(chat::MessageStatus::kStored));
    assert(message_repo.created_messages.size() == 1);
    assert(message_repo.delivered_message_ids.empty());
}

void TestSendMessageStoreFailureReturnsError() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    message_repo.create_status = chat::RepositoryStatus::kInsertFailed;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_store_failure";
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::DB_INSERT_FAILED);
    assert(result.message == "store message failed");
    assert(message_repo.created_messages.empty());
}

void TestSendMessageRejectsEmptyClientMsgId() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
    assert(message_repo.created_messages.empty());
}

void TestSendMessageRejectsLongClientMsgId() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = std::string(65, 'x');
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
    assert(message_repo.created_messages.empty());
}

void TestSendMessageDoesNotMarkDeliveredBeforePushAck() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    message_repo.mark_delivered_status = chat::RepositoryStatus::kQueryFailed;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::ConnectionSession receiver_session;
    receiver_session.authenticated = true;
    receiver_session.user_id = 2;
    receiver_session.username = "receiver";
    session_manager.BindSession(200, receiver_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_delivery_fail";
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.to_conn_id == 200);
    assert(result.status == chat::ToProtocolMessageStatus(chat::MessageStatus::kStored));
    assert(message_repo.delivered_message_ids.empty());
}

void TestSendMessageSuccess() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

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
    req.client_msg_id = "msg_test_1";
    req.to_user_id = 2;
    req.content = "hello world";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.from_user_id == 1);
    assert(result.from_username == "sender");
    assert(result.to_user_id == 2);
    assert(result.to_conn_id == 200);
    assert(result.content == "hello world");
    assert(result.server_time > 0);
    assert(!result.message_id.empty());
    assert(result.conversation_id == "conv_1_2");
    assert(result.status == chat::ToProtocolMessageStatus(chat::MessageStatus::kStored));
    assert(result.created_at == result.server_time);
    assert(message_repo.created_messages.size() == 1);
    assert(message_repo.created_messages[0].status == chat::MessageStatus::kStored);
    assert(message_repo.delivered_message_ids.empty());
}

void TestSendMessageDuplicateClientMsgIdReturnsSameMessage() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_duplicate_same";
    req.to_user_id = 2;
    req.content = "hello";

    auto first = chat_service.sendMessage(100, req);
    auto second = chat_service.sendMessage(100, req);

    assert(first.code == chat::ErrorCode::OK);
    assert(second.code == chat::ErrorCode::OK);
    assert(!first.message_id.empty());
    assert(second.message_id == first.message_id);
    assert(message_repo.created_messages.size() == 1);
}

void TestSendMessageDeduplicatedDeliveredDoesNotPushAgain() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::MessageRecord existing;
    existing.id = "m_existing";
    existing.conversation_id = "conv_1_2";
    existing.client_msg_id = "msg_duplicate";
    existing.from_user_id = 1;
    existing.to_user_id = 2;
    existing.content = "hello";
    existing.status = chat::MessageStatus::kDelivered;
    existing.created_at = 12345;
    message_repo.create_message_override = existing;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::ConnectionSession receiver_session;
    receiver_session.authenticated = true;
    receiver_session.user_id = 2;
    receiver_session.username = "receiver";
    session_manager.BindSession(200, receiver_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_duplicate";
    req.to_user_id = 2;
    req.content = "hello";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.to_conn_id == 0);
    assert(result.message_id == "m_existing");
    assert(result.status == chat::ToProtocolMessageStatus(chat::MessageStatus::kDelivered));
    assert(message_repo.delivered_message_ids.empty());
}

void TestSendMessageDeduplicatedConflict() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::MessageRecord existing;
    existing.id = "m_existing";
    existing.conversation_id = "conv_1_3";
    existing.client_msg_id = "msg_duplicate";
    existing.from_user_id = 1;
    existing.to_user_id = 3;
    existing.content = "old target";
    existing.status = chat::MessageStatus::kStored;
    existing.created_at = 12345;
    message_repo.create_message_override = existing;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession sender_session;
    sender_session.authenticated = true;
    sender_session.user_id = 1;
    sender_session.username = "sender";
    session_manager.BindSession(100, sender_session);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_duplicate";
    req.to_user_id = 2;
    req.content = "new target";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::IDEMPOTENCY_CONFLICT);
    assert(message_repo.delivered_message_ids.empty());
}

void TestSendMessageEmptyContent() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_empty_content";
    req.to_user_id = 2;
    req.content = "";

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
}

void TestSendMessageTooLongContent() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::SendMessageRequest req;
    req.client_msg_id = "msg_too_long";
    req.to_user_id = 2;
    req.content = std::string(4097, 'x');

    auto result = chat_service.sendMessage(100, req);
    assert(result.code == chat::ErrorCode::MESSAGE_TOO_LONG);
}

void TestPullOfflineMessagesNotLoggedIn() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::NOT_LOGGED_IN);
}

void TestPullOfflineMessagesSuccess() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    // Setup user session
    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::MessageRecord offline;
    offline.id = "m1";
    offline.conversation_id = "conv_1_2";
    offline.client_msg_id = "c1";
    offline.from_user_id = 2;
    offline.to_user_id = 1;
    offline.content = "offline";
    offline.status = chat::MessageStatus::kStored;
    offline.created_at = 12345;
    message_repo.list_result.messages.push_back(offline);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;
    req.since_message_id = "msg_last";

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.messages.size() == 1);
    assert(result.messages[0].message_id == "m1");
    assert(result.messages[0].status == chat::ToProtocolMessageStatus(chat::MessageStatus::kStored));
    assert(result.has_more == false);
    assert(message_repo.delivered_message_ids.empty());
    assert(message_repo.last_since_message_id == "msg_last");
}

void TestPullOfflineMessagesFiltersToLoggedInUser() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::MessageRecord own_message;
    own_message.id = "m_own";
    own_message.conversation_id = "conv_1_2";
    own_message.client_msg_id = "c_own";
    own_message.from_user_id = 2;
    own_message.to_user_id = 1;
    own_message.content = "for user1";
    own_message.status = chat::MessageStatus::kStored;
    own_message.created_at = 12345;
    message_repo.created_messages.push_back(own_message);

    chat::MessageRecord other_message = own_message;
    other_message.id = "m_other";
    other_message.client_msg_id = "c_other";
    other_message.to_user_id = 2;
    other_message.content = "for user2";
    message_repo.created_messages.push_back(other_message);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.messages.size() == 1);
    assert(result.messages[0].message_id == "m_own");
    assert(result.messages[0].to_user_id == 1);
}

void TestPullOfflineMessagesDoesNotMarkDeliveredBeforeClientAck() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    message_repo.mark_delivered_status = chat::RepositoryStatus::kQueryFailed;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::MessageRecord offline;
    offline.id = "m1";
    offline.conversation_id = "conv_1_2";
    offline.client_msg_id = "c1";
    offline.from_user_id = 2;
    offline.to_user_id = 1;
    offline.content = "offline";
    offline.status = chat::MessageStatus::kStored;
    offline.created_at = 12345;
    message_repo.list_result.messages.push_back(offline);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.messages.size() == 1);
    assert(result.messages[0].status == chat::ToProtocolMessageStatus(chat::MessageStatus::kStored));
    assert(message_repo.delivered_message_ids.empty());
}

void TestPullOfflineMessagesReturnsAllWithoutDeliveryMutation() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    message_repo.mark_delivered_fail_after = 1;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::MessageRecord first;
    first.id = "m1";
    first.conversation_id = "conv_1_2";
    first.client_msg_id = "c1";
    first.from_user_id = 2;
    first.to_user_id = 1;
    first.content = "first";
    first.status = chat::MessageStatus::kStored;
    first.created_at = 12345;
    message_repo.list_result.messages.push_back(first);

    chat::MessageRecord second = first;
    second.id = "m2";
    second.client_msg_id = "c2";
    second.content = "second";
    message_repo.list_result.messages.push_back(second);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.messages.size() == 2);
    assert(result.messages[0].message_id == "m1");
    assert(result.messages[1].message_id == "m2");
    assert(result.has_more == false);
    assert(message_repo.delivered_message_ids.empty());
}

void TestPullOfflineMessagesRejectsTwoCursors() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::PullOfflineMessagesRequest req;
    req.limit = 10;
    req.before_message_id = "before";
    req.since_message_id = "since";

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
}

void TestPullOfflineMessagesRejectsOversizedLimit() {
    FakeSessionManager session_manager;
    FakeMessageRepository message_repo;
    FakeUserRepository user_repo;
    chat::ChatService chat_service(session_manager, message_repo, user_repo);

    chat::ConnectionSession user_session;
    user_session.authenticated = true;
    user_session.user_id = 1;
    user_session.username = "user1";
    session_manager.BindSession(100, user_session);

    chat::PullOfflineMessagesRequest req;
    req.limit = 101;

    auto result = chat_service.pullOfflineMessages(100, req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
}

}  // namespace

int main() {
    TestSendMessageNotLoggedIn();
    TestSendMessageToSelf();
    TestSendMessageTargetNotExists();
    TestSendMessageTargetNotOnline();
    TestSendMessageStoreFailureReturnsError();
    TestSendMessageRejectsEmptyClientMsgId();
    TestSendMessageRejectsLongClientMsgId();
    TestSendMessageDoesNotMarkDeliveredBeforePushAck();
    TestSendMessageSuccess();
    TestSendMessageDuplicateClientMsgIdReturnsSameMessage();
    TestSendMessageDeduplicatedDeliveredDoesNotPushAgain();
    TestSendMessageDeduplicatedConflict();
    TestSendMessageEmptyContent();
    TestSendMessageTooLongContent();
    TestPullOfflineMessagesNotLoggedIn();
    TestPullOfflineMessagesSuccess();
    TestPullOfflineMessagesFiltersToLoggedInUser();
    TestPullOfflineMessagesDoesNotMarkDeliveredBeforeClientAck();
    TestPullOfflineMessagesReturnsAllWithoutDeliveryMutation();
    TestPullOfflineMessagesRejectsTwoCursors();
    TestPullOfflineMessagesRejectsOversizedLimit();
    std::cout << "[PASS] chat service tests passed\n";
    return 0;
}
