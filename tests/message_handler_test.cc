#include "handler/message_handler.h"
#include "service/chat_service.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "common/error_code.h"
#include "db/user_repository.h"

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

  CreateUserResult createUser(const std::string& username,
                              const std::string& password_hash,
                              const std::string& nickname) override {
    (void)username;
    (void)password_hash;
    (void)nickname;
    return {};
  }
};

std::string HashPasswordForTest(const std::string& password) {
  return std::to_string(std::hash<std::string>{}(password));
}

nlohmann::json ParseResponse(const std::string& raw_response) {
  assert(!raw_response.empty());
  return nlohmann::json::parse(raw_response);
}

nlohmann::json ParseResponse(const HandleResult& result) {
  return ParseResponse(result.response);
}

HandleResult DispatchAndApply(MessageHandler& handler,
                              const std::string& raw_request,
                              ConnectionId conn_id) {
  HandleResult result = handler.handle(raw_request, conn_id);
  if (result.session_action == SessionAction::BIND) {
    handler.applyBindSession(conn_id, result.pending_session);
  } else if (result.session_action == SessionAction::UNBIND) {
    handler.applyUnbindSession(conn_id);
  }
  return result;
}

void ExpectCommonEnvelope(const nlohmann::json& resp, const std::string& msg_type,
                          int seq, ErrorCode code) {
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
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"heartbeat","seq":1,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 1));
  ExpectCommonEnvelope(resp, "heartbeat_resp", 1, ErrorCode::OK);
  assert(resp["message"].get<std::string>() == "Heartbeat received");
  assert(resp["data"].contains("server_time"));
  assert(resp["data"]["server_time"].is_number_integer());
}

void TestHandleLoginDbQueryFailedWithoutDbConfig() {
  FakeUserRepository repo;
  repo.find_by_username_result.status = RepositoryStatus::kQueryFailed;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
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
  ChatService chat_service(session_manager);
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
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"logout","seq":4,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 4));
  ExpectCommonEnvelope(resp, "logout_resp", 4, ErrorCode::USER_NOT_FOUND);
  assert(resp["message"].get<std::string>() == "user not logged in");
}

void TestHandleWhoAmIReturnsUserNotLoggedIn() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"whoami","seq":5,"token":"","data":{}})";

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
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);

  const HandleResult login_result = DispatchAndApply(
      handler,
      R"({"msg_type":"login","seq":6,"token":"","data":{"username":"alice","password":"123456"}})",
      42);
  assert(login_result.session_action == SessionAction::BIND);
  const nlohmann::json login_resp = ParseResponse(login_result);
  ExpectCommonEnvelope(login_resp, "login_resp", 6, ErrorCode::OK);
  assert(login_resp["data"]["user_id"].get<int>() == 10001);
  assert(login_resp["data"]["token"].get<std::string>() == "token_10001");

  const nlohmann::json whoami_resp = ParseResponse(
      DispatchAndApply(handler, R"({"msg_type":"whoami","seq":7,"token":"","data":{}})", 42));
  ExpectCommonEnvelope(whoami_resp, "whoami_resp", 7, ErrorCode::OK);
  assert(whoami_resp["data"]["user_id"].get<int>() == 10001);
  assert(whoami_resp["data"]["username"].get<std::string>() == "alice");
  assert(whoami_resp["data"]["token"].get<std::string>() == "token_10001");

  const nlohmann::json other_conn_resp = ParseResponse(
      DispatchAndApply(handler, R"({"msg_type":"whoami","seq":8,"token":"","data":{}})", 77));
  ExpectCommonEnvelope(other_conn_resp, "whoami_resp", 8,
                       ErrorCode::USER_NOT_FOUND);

  const HandleResult logout_result =
      DispatchAndApply(handler, R"({"msg_type":"logout","seq":9,"token":"","data":{}})", 42);
  assert(logout_result.session_action == SessionAction::UNBIND);
  const nlohmann::json logout_resp = ParseResponse(logout_result);
  ExpectCommonEnvelope(logout_resp, "logout_resp", 9, ErrorCode::OK);
  assert(logout_resp["message"].get<std::string>() == "logout success");

  const nlohmann::json whoami_after_logout = ParseResponse(
      DispatchAndApply(handler, R"({"msg_type":"whoami","seq":10,"token":"","data":{}})", 42));
  ExpectCommonEnvelope(whoami_after_logout, "whoami_resp", 10,
                       ErrorCode::USER_NOT_FOUND);
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
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);

  const nlohmann::json login_resp = ParseResponse(DispatchAndApply(
      handler,
      R"({"msg_type":"login","seq":17,"token":"","data":{"username":"alice","password":"123456"}})",
      42));
  ExpectCommonEnvelope(login_resp, "login_resp", 17, ErrorCode::OK);

  handler.onConnectionClosed(42);

  const nlohmann::json whoami_after_close = ParseResponse(
      DispatchAndApply(handler, R"({"msg_type":"whoami","seq":18,"token":"","data":{}})", 42));
  ExpectCommonEnvelope(whoami_after_close, "whoami_resp", 18,
                       ErrorCode::USER_NOT_FOUND);
}

void TestHandleUnknownMessageType() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"chat","seq":11,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 11));
  ExpectCommonEnvelope(resp, "chat_resp", 11, ErrorCode::UNKNOWN_MESSAGE_TYPE);
  assert(resp["message"].get<std::string>() == "Unknown message type: chat");
}

void TestHandleLoginMissingPassword() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"login","seq":12,"token":"","data":{"username":"alice"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 12));
  ExpectCommonEnvelope(resp, "login_resp", 12, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("password") !=
         std::string::npos);
}

void TestHandleRegisterMissingUsername() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"register","seq":13,"token":"","data":{"password":"123456"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 13));
  ExpectCommonEnvelope(resp, "register_resp", 13, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("username") !=
         std::string::npos);
}

void TestHandleInvalidDataFieldType() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request =
      R"({"msg_type":"login","seq":14,"token":"","data":"bad"})";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 14));
  ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("data") != std::string::npos);
}

void TestHandleInvalidSeqType() {
  FakeUserRepository repo;
  SessionManager session_manager;
  UserService service(repo, session_manager);
  ChatService chat_service(session_manager);
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
  ChatService chat_service(session_manager);
  MessageHandler handler(service, chat_service);
  const std::string request = R"({"msg_type":"login","seq":16,"data":)";

  const nlohmann::json resp = ParseResponse(handler.handle(request, 16));
  ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("JSON") != std::string::npos);
}

}  // namespace

int main() {
  TestHandleHeartbeatSuccess();
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

  std::cout << "[PASS] message handler tests passed\n";
  return 0;
}
