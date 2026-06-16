#include <cassert>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "fake_message_repository.h"
#include "fake_user_repository.h"
#include "server/session_manager.h"
#include "service/chat_service.h"

namespace {

void BindSender(chat::SessionManager* sessions) {
    chat::ConnectionSession sender;
    sender.authenticated = true;
    sender.user_id = 1;
    sender.username = "sender";
    sender.token = std::string(64, 'a');
    assert(sessions->BindSession(100, sender));
}

void TestInvalidSendInputDoesNotReachRepositories() {
    const std::vector<std::pair<std::string, std::string>> invalid = {
        {"", "content"},
        {std::string(65, 'a'), "content"},
        {"bad'id", "content"},
        {"bad\\id", "content"},
        {"bad id", "content"},
        {"client_id", ""},
        {"client_id", std::string(4097, 'x')},
        {"client_id", std::string("abc\0def", 7)},
    };
    for (const auto& test : invalid) {
        chat::SessionManager sessions;
        BindSender(&sessions);
        FakeMessageRepository messages;
        FakeUserRepository users;
        chat::ChatService service(sessions, messages, users);
        chat::SendMessageRequest request;
        request.client_msg_id = test.first;
        request.to_user_id = 2;
        request.content = test.second;

        const auto result = service.sendMessage(100, request);
        assert(result.code == chat::ErrorCode::INVALID_PARAM || result.code == chat::ErrorCode::MESSAGE_TOO_LONG);
        assert(users.find_by_id_calls == 0);
        assert(messages.create_message_calls == 0);
        assert(messages.find_by_client_msg_id_calls == 0);
    }
}

void TestAllowedMessageCharactersReachRepositoryUnchanged() {
    chat::SessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::ChatService service(sessions, messages, users);
    chat::SendMessageRequest request;
    request.client_msg_id = "client_id";
    request.to_user_id = 2;
    request.content = "quote'\" slash\\ newline\n unicode \xe4\xbd\xa0\xe5\xa5\xbd";

    const auto result = service.sendMessage(100, request);
    assert(result.code == chat::ErrorCode::OK);
    assert(users.find_by_id_calls == 1);
    assert(messages.create_message_calls == 1);
    assert(messages.created_messages[0].content == request.content);
}

void TestInvalidCursorAndMessageIdsDoNotReachRepository() {
    const std::vector<std::string> invalid_cursors = {
        "bad'cursor",
        "bad cursor",
        "bad\\cursor",
        std::string(65, 'x'),
    };
    for (const std::string& cursor : invalid_cursors) {
        chat::SessionManager sessions;
        BindSender(&sessions);
        FakeMessageRepository messages;
        FakeUserRepository users;
        chat::ChatService service(sessions, messages, users);
        chat::PullOfflineMessagesRequest request;
        request.limit = 10;
        request.since_message_id = cursor;

        assert(service.pullOfflineMessages(100, request).code == chat::ErrorCode::INVALID_PARAM);
        assert(messages.list_offline_calls == 0);
    }

    chat::SessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::ChatService service(sessions, messages, users);
    service.markMessagesDelivered(1, {"safe_id", "' OR 1=1 --"});
    assert(messages.delivered_message_ids.empty());
    service.markMessagesDelivered(1, {"safe_id"});
    assert(messages.delivered_message_ids.size() == 1);
}

}  // namespace

int main() {
    TestInvalidSendInputDoesNotReachRepositories();
    TestAllowedMessageCharactersReachRepositoryUnchanged();
    TestInvalidCursorAndMessageIdsDoNotReachRepository();
    std::cout << "[PASS] chat service validation tests passed\n";
    return 0;
}
