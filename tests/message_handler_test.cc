#include "handler/message_handler.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "common/error_code.h"
#include "db/user_repository.h"
#include "fake_message_repository.h"
#include "service/chat_service.h"

using namespace chat;

namespace {

class FakeUserRepository : public IUserRepository {
   public:
    FindUserResult find_by_username_result;
    FindUserResult find_by_id_result;

    FindUserResult findByUsername(const std::string& username) override {
        (void)username;
        return find_by_username_result;
    }

    FindUserResult findById(UserId user_id) override {
        (void)user_id;
        return find_by_id_result;
    }

    CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                const std::string& nickname) override {
        (void)username;
        (void)password_hash;
        (void)nickname;
        return {};
    }

    RepositoryStatus updatePasswordHash(UserId, const std::string&) override { return RepositoryStatus::kOk; }
};

class CountingGlobalSessionStore : public IGlobalSessionStore {
   public:
    bool Bind(ConnectionId connection_id, const ConnectionSession& session, Timestamp issued_at) override {
        (void)connection_id;
        (void)session;
        (void)issued_at;
        ++bind_count;
        return true;
    }

    bool Refresh(ConnectionId connection_id, const ConnectionSession& session) override {
        (void)connection_id;
        (void)session;
        ++refresh_count;
        return true;
    }

    bool ClearPresence(ConnectionId connection_id, const ConnectionSession& session) override {
        (void)connection_id;
        (void)session;
        return true;
    }

    bool RevokeToken(const std::string& token) override {
        (void)token;
        return true;
    }

    std::optional<StoredSessionToken> GetToken(const std::string& token) override {
        (void)token;
        return std::nullopt;
    }

    std::optional<StoredUserPresence> GetPresence(UserId user_id) override {
        (void)user_id;
        return std::nullopt;
    }

    int bind_count = 0;
    int refresh_count = 0;
};

std::string HashPasswordForTest(const std::string& password) {
    return std::to_string(std::hash<std::string>{}(password));
}

nlohmann::json ParseResponse(const std::string& raw_response) {
    assert(!raw_response.empty());
    return nlohmann::json::parse(raw_response);
}

nlohmann::json ParseResponse(const HandleResult& result) { return ParseResponse(result.response); }

HandleResult DispatchAndApply(MessageHandler& handler, const std::string& raw_request, ConnectionId conn_id) {
    HandleResult result = handler.handle(raw_request, conn_id);
    if (result.session_action == SessionAction::BIND) {
        handler.applyBindSession(conn_id, result.pending_session);
    } else if (result.session_action == SessionAction::UNBIND) {
        handler.applyUnbindSession(conn_id);
    }
    return result;
}

void ExpectCommonEnvelope(const nlohmann::json& resp, const std::string& msg_type, int seq, ErrorCode code) {
    assert(resp.contains("msg_type"));
    assert(resp.contains("seq"));
    assert(resp.contains("code"));
    assert(resp.contains("message"));
    assert(resp.contains("data"));

    assert(resp["msg_type"].is_string());
    assert(resp["seq"].is_number_integer());
    assert(resp["code"].is_number_integer());
    assert(resp["message"].is_string());
    assert(resp["data"].is_object());

    assert(resp["msg_type"].get<std::string>() == msg_type);
    assert(resp["seq"].get<int>() == seq);
    assert(resp["code"].get<int>() == static_cast<int>(code));
}

void TestHandleHeartbeatSuccess() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"heartbeat","seq":1,"token":"","data":{}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 1));
    ExpectCommonEnvelope(resp, "heartbeat_resp", 1, ErrorCode::OK);
    assert(resp["message"].get<std::string>() == "Heartbeat received");
    assert(resp["data"].contains("server_time"));
    assert(resp["data"]["server_time"].is_number_integer());
}

void TestAnyValidRequestRefreshesPresence() {
    FakeUserRepository repo;
    SessionManager session_manager;
    CountingGlobalSessionStore global_store;
    UserService service(repo, session_manager, &global_store);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    ConnectionSession session{true, 10001, "alice", "token"};
    handler.applyBindSession(42, session);
    assert(global_store.bind_count == 1);

    handler.handle(R"({"msg_type":"heartbeat","seq":1,"token":"","data":{}})", 42);
    handler.handle(R"({"msg_type":"unknown_valid","seq":2,"token":"","data":{}})", 42);
    assert(global_store.refresh_count == 2);

    handler.handle(R"({"msg_type":"heartbeat","seq":)", 42);
    assert(global_store.refresh_count == 2);
}

void TestHandleLoginDbQueryFailedWithoutDbConfig() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = RepositoryStatus::kQueryFailed;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request =
        R"({"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 2));
    ExpectCommonEnvelope(resp, "login_resp", 2, ErrorCode::DB_QUERY_FAILED);
    assert(resp["message"].get<std::string>() == "query user failed");
}

void TestHandleRegisterDbQueryFailedWithoutDbConfig() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = RepositoryStatus::kQueryFailed;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request =
        R"({"msg_type":"register","seq":3,"token":"","data":{"username":"alice","password":"123456","nickname":"Alice"}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 3));
    ExpectCommonEnvelope(resp, "register_resp", 3, ErrorCode::DB_QUERY_FAILED);
    assert(resp["message"].get<std::string>() == "query user failed");
}

void TestHandleLogoutReturnsUserNotLoggedIn() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"logout","seq":4,"token":"","data":{}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 4));
    ExpectCommonEnvelope(resp, "logout_resp", 4, ErrorCode::USER_NOT_FOUND);
    assert(resp["message"].get<std::string>() == "user not logged in");
}

void TestHandleWhoAmIReturnsUserNotLoggedIn() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"whoami","seq":5,"token":"","data":{}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 5));
    ExpectCommonEnvelope(resp, "whoami_resp", 5, ErrorCode::USER_NOT_FOUND);
    assert(resp["message"].get<std::string>() == "user not logged in");
}

void TestHandleLoginThenWhoAmIThenLogoutOnSameConnection() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserRecord record;
    record.id = 10001;
    record.username = "alice";
    record.nickname = "Alice";
    record.password_hash = HashPasswordForTest("123456");
    repo.find_by_username_result.status = RepositoryStatus::kOk;
    repo.find_by_username_result.user = record;

    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    const HandleResult login_result = DispatchAndApply(
        handler, R"({"msg_type":"login","seq":6,"token":"","data":{"username":"alice","password":"123456"}})", 42);
    assert(login_result.session_action == SessionAction::BIND);
    const nlohmann::json login_resp = ParseResponse(login_result);
    ExpectCommonEnvelope(login_resp, "login_resp", 6, ErrorCode::OK);
    assert(login_resp["data"]["user_id"].get<int>() == 10001);
    const std::string login_token = login_resp["data"]["token"].get<std::string>();
    assert(login_token.size() == 64);

    const nlohmann::json whoami_resp =
        ParseResponse(DispatchAndApply(handler, R"({"msg_type":"whoami","seq":7,"token":"","data":{}})", 42));
    ExpectCommonEnvelope(whoami_resp, "whoami_resp", 7, ErrorCode::OK);
    assert(whoami_resp["data"]["user_id"].get<int>() == 10001);
    assert(whoami_resp["data"]["username"].get<std::string>() == "alice");
    assert(whoami_resp["data"]["token"].get<std::string>() == login_token);

    const nlohmann::json other_conn_resp =
        ParseResponse(DispatchAndApply(handler, R"({"msg_type":"whoami","seq":8,"token":"","data":{}})", 77));
    ExpectCommonEnvelope(other_conn_resp, "whoami_resp", 8, ErrorCode::USER_NOT_FOUND);

    const HandleResult logout_result =
        DispatchAndApply(handler, R"({"msg_type":"logout","seq":9,"token":"","data":{}})", 42);
    assert(logout_result.session_action == SessionAction::UNBIND);
    const nlohmann::json logout_resp = ParseResponse(logout_result);
    ExpectCommonEnvelope(logout_resp, "logout_resp", 9, ErrorCode::OK);
    assert(logout_resp["message"].get<std::string>() == "logout success");

    const nlohmann::json whoami_after_logout =
        ParseResponse(DispatchAndApply(handler, R"({"msg_type":"whoami","seq":10,"token":"","data":{}})", 42));
    ExpectCommonEnvelope(whoami_after_logout, "whoami_resp", 10, ErrorCode::USER_NOT_FOUND);
}

void TestHandleConnectionClosedClearsSession() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserRecord record;
    record.id = 10001;
    record.username = "alice";
    record.nickname = "Alice";
    record.password_hash = HashPasswordForTest("123456");
    repo.find_by_username_result.status = RepositoryStatus::kOk;
    repo.find_by_username_result.user = record;

    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    const nlohmann::json login_resp = ParseResponse(DispatchAndApply(
        handler, R"({"msg_type":"login","seq":17,"token":"","data":{"username":"alice","password":"123456"}})", 42));
    ExpectCommonEnvelope(login_resp, "login_resp", 17, ErrorCode::OK);

    handler.onConnectionClosed(42);

    const nlohmann::json whoami_after_close =
        ParseResponse(DispatchAndApply(handler, R"({"msg_type":"whoami","seq":18,"token":"","data":{}})", 42));
    ExpectCommonEnvelope(whoami_after_close, "whoami_resp", 18, ErrorCode::USER_NOT_FOUND);
}

void TestHandleUnknownMessageType() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"chat","seq":11,"token":"","data":{}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 11));
    ExpectCommonEnvelope(resp, "chat_resp", 11, ErrorCode::UNKNOWN_MESSAGE_TYPE);
    assert(resp["message"].get<std::string>() == "Unknown message type: chat");
}

void TestHandleLoginMissingPassword() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"login","seq":12,"token":"","data":{"username":"alice"}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 12));
    ExpectCommonEnvelope(resp, "login_resp", 12, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("password") != std::string::npos);
}

void TestHandleRegisterMissingUsername() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"register","seq":13,"token":"","data":{"password":"123456"}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 13));
    ExpectCommonEnvelope(resp, "register_resp", 13, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("username") != std::string::npos);
}

void TestHandleInvalidDataFieldType() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"login","seq":14,"token":"","data":"bad"})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 14));
    ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("data") != std::string::npos);
}

void TestHandleInvalidSeqType() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request =
        R"({"msg_type":"login","seq":"15","token":"","data":{"username":"alice","password":"123456"}})";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 15));
    ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("seq") != std::string::npos);
}

void TestHandleInvalidJson() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);
    const std::string request = R"({"msg_type":"login","seq":16,"data":)";

    const nlohmann::json resp = ParseResponse(handler.handle(request, 16));
    ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("JSON") != std::string::npos);
}

void TestHandleSendMessageSuccess() {
    FakeUserRepository repo;
    UserRecord target_user;
    target_user.id = 10002;
    repo.find_by_id_result.status = RepositoryStatus::kOk;
    repo.find_by_id_result.user = target_user;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    // Setup alice session on conn 42
    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    // Setup bob session on conn 43
    ConnectionSession bob_session;
    bob_session.authenticated = true;
    bob_session.user_id = 10002;
    bob_session.username = "bob";
    session_manager.BindSession(43, bob_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":20,"token":"","data":{"client_msg_id":"cmsg_100","to_user_id":10002,"content":"hello bob"}})";

    const HandleResult result = handler.handle(request, 42);

    // Verify ack to Alice
    const nlohmann::json ack_resp = ParseResponse(result);
    ExpectCommonEnvelope(ack_resp, "send_message_resp", 20, ErrorCode::OK);
    assert(ack_resp["data"]["to_user_id"].get<UserId>() == 10002);
    assert(!ack_resp["data"]["message_id"].get<std::string>().empty());
    assert(ack_resp["data"]["conversation_id"].get<std::string>() == "conv_10001_10002");
    assert(ack_resp["data"]["status"].get<int32_t>() == 0);
    assert(ack_resp["data"]["created_at"].is_number_integer());

    // Verify push to Bob
    assert(result.pushes.size() == 1);
    assert(result.pushes[0].target_conn_id == 43);

    const nlohmann::json push_resp = ParseResponse(result.pushes[0].payload);
    ExpectCommonEnvelope(push_resp, "message_push", 0, ErrorCode::OK);
    assert(!push_resp["data"]["message_id"].get<std::string>().empty());
    assert(push_resp["data"]["conversation_id"].get<std::string>() == "conv_10001_10002");
    assert(push_resp["data"]["from_user_id"].get<UserId>() == 10001);
    assert(push_resp["data"]["from_username"].get<std::string>() == "alice");
    assert(push_resp["data"]["to_user_id"].get<UserId>() == 10002);
    assert(push_resp["data"]["content"].get<std::string>() == "hello bob");
    assert(push_resp["data"]["created_at"].is_number_integer());
    assert(push_resp["data"]["server_time"].is_number_integer());
}

void TestSendMessagePushDiscardedOnConnectionRebound() {
    FakeUserRepository repo;
    UserRecord target_user;
    target_user.id = 10002;
    repo.find_by_id_result.status = RepositoryStatus::kOk;
    repo.find_by_id_result.user = target_user;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    // Setup alice session on conn 42
    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    // Setup bob session on conn 43
    ConnectionSession bob_session;
    bob_session.authenticated = true;
    bob_session.user_id = 10002;
    bob_session.username = "bob";
    session_manager.BindSession(43, bob_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":20,"token":"","data":{"client_msg_id":"cmsg_200","to_user_id":10002,"content":"hello bob"}})";

    const HandleResult result = handler.handle(request, 42);

    // Verify ack to Alice
    const nlohmann::json ack_resp = ParseResponse(result);
    ExpectCommonEnvelope(ack_resp, "send_message_resp", 20, ErrorCode::OK);

    // Verify push is generated for Bob
    assert(result.pushes.size() == 1);
    const auto& push = result.pushes[0];
    assert(push.target_conn_id == 43);
    assert(push.target_user_id == 10002);

    // Assert Bob is currently bound
    assert(handler.isConnectionBoundToUser(push.target_conn_id, push.target_user_id) == true);

    // Rebind connection 43 to Charlie (user_id = 10003)
    ConnectionSession charlie_session;
    charlie_session.authenticated = true;
    charlie_session.user_id = 10003;
    charlie_session.username = "charlie";
    session_manager.BindSession(43, charlie_session);

    // Assert Bob is NO LONGER bound to connection 43
    assert(handler.isConnectionBoundToUser(push.target_conn_id, push.target_user_id) == false);

    // Clear connection 43 session completely
    session_manager.ClearSession(43);

    // Assert Bob is STILL NOT bound to connection 43
    assert(handler.isConnectionBoundToUser(push.target_conn_id, push.target_user_id) == false);
}

void TestHandleSendMessageOfflineSuccess() {
    FakeUserRepository repo;
    UserRecord target_user;
    target_user.id = 10002;
    repo.find_by_id_result.status = RepositoryStatus::kOk;
    repo.find_by_id_result.user = target_user;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":23,"token":"","data":{"client_msg_id":"cmsg_offline","to_user_id":10002,"content":"stored for bob"}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json ack_resp = ParseResponse(result);
    ExpectCommonEnvelope(ack_resp, "send_message_resp", 23, ErrorCode::OK);
    assert(!ack_resp["data"]["message_id"].get<std::string>().empty());
    assert(ack_resp["data"]["status"].get<int32_t>() == 0);
    assert(result.pushes.empty());
    assert(message_repo.created_messages.size() == 1);
    assert(message_repo.created_messages[0].to_user_id == 10002);
}

void TestHandleSendMessageStoreFailure() {
    FakeUserRepository repo;
    UserRecord target_user;
    target_user.id = 10002;
    repo.find_by_id_result.status = RepositoryStatus::kOk;
    repo.find_by_id_result.user = target_user;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    message_repo.create_status = RepositoryStatus::kInsertFailed;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":24,"token":"","data":{"client_msg_id":"cmsg_store_fail","to_user_id":10002,"content":"will fail"}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json resp = ParseResponse(result);
    ExpectCommonEnvelope(resp, "send_message_resp", 24, ErrorCode::DB_INSERT_FAILED);
    assert(resp["message"].get<std::string>() == "store message failed");
    assert(resp["data"].empty());
    assert(result.pushes.empty());
}

void TestHandleSendMessageMissingClientMsgId() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    // Setup alice session on conn 42
    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":21,"token":"","data":{"to_user_id":10002,"content":"hello bob"}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json resp = ParseResponse(result);
    ExpectCommonEnvelope(resp, "send_message_resp", 21, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("client_msg_id") != std::string::npos);
}

void TestHandleSendMessageEmptyClientMsgId() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    // Setup alice session on conn 42
    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    const std::string request =
        R"({"msg_type":"send_message","seq":22,"token":"","data":{"client_msg_id":"","to_user_id":10002,"content":"hello bob"}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json resp = ParseResponse(result);
    ExpectCommonEnvelope(resp, "send_message_resp", 22, ErrorCode::INVALID_PARAM);
    assert(resp["message"].get<std::string>().find("empty") != std::string::npos);
}

void TestHandlePullOfflineMessagesNotLoggedIn() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    const std::string request = R"({"msg_type":"pull_offline_messages","seq":30,"token":"","data":{"limit":10}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json resp = ParseResponse(result);
    ExpectCommonEnvelope(resp, "pull_offline_messages_resp", 30, ErrorCode::NOT_LOGGED_IN);
}

void TestHandlePullOfflineMessagesSuccess() {
    FakeUserRepository repo;
    SessionManager session_manager;
    UserService service(repo, session_manager);
    FakeMessageRepository message_repo;
    ChatService chat_service(session_manager, message_repo, repo);
    MessageHandler handler(service, chat_service);

    // Setup alice session on conn 42
    ConnectionSession alice_session;
    alice_session.authenticated = true;
    alice_session.user_id = 10001;
    alice_session.username = "alice";
    session_manager.BindSession(42, alice_session);

    const std::string request =
        R"({"msg_type":"pull_offline_messages","seq":31,"token":"","data":{"limit":15,"since_message_id":"msg_123"}})";

    const HandleResult result = handler.handle(request, 42);
    const nlohmann::json resp = ParseResponse(result);
    ExpectCommonEnvelope(resp, "pull_offline_messages_resp", 31, ErrorCode::OK);
    assert(resp["data"]["messages"].is_array());
    assert(resp["data"]["messages"].empty());
    assert(resp["data"]["has_more"].get<bool>() == false);
}

}  // namespace

int main() {
    TestHandleHeartbeatSuccess();
    TestAnyValidRequestRefreshesPresence();
    TestHandleLoginDbQueryFailedWithoutDbConfig();
    TestHandleRegisterDbQueryFailedWithoutDbConfig();
    TestHandleLogoutReturnsUserNotLoggedIn();
    TestHandleWhoAmIReturnsUserNotLoggedIn();
    TestHandleLoginThenWhoAmIThenLogoutOnSameConnection();
    TestHandleConnectionClosedClearsSession();
    TestHandleUnknownMessageType();
    TestHandleLoginMissingPassword();
    TestHandleRegisterMissingUsername();
    TestHandleInvalidDataFieldType();
    TestHandleInvalidSeqType();
    TestHandleInvalidJson();
    TestHandleSendMessageSuccess();
    TestSendMessagePushDiscardedOnConnectionRebound();
    TestHandleSendMessageOfflineSuccess();
    TestHandleSendMessageStoreFailure();
    TestHandleSendMessageMissingClientMsgId();
    TestHandleSendMessageEmptyClientMsgId();
    TestHandlePullOfflineMessagesNotLoggedIn();
    TestHandlePullOfflineMessagesSuccess();

    std::cout << "[PASS] message handler tests passed\n";
    return 0;
}
