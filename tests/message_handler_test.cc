#include "handler/message_handler.h"

#include <cassert>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "common/error_code.h"

using namespace chat;

namespace {

nlohmann::json ParseResponse(const std::string& raw_response) {
  assert(!raw_response.empty());
  return nlohmann::json::parse(raw_response);
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
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"heartbeat","seq":1,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "heartbeat_resp", 1, ErrorCode::OK);
  assert(resp["message"].get<std::string>() == "Heartbeat received");
  assert(resp["data"].contains("server_time"));
  assert(resp["data"]["server_time"].is_number_integer());
}

void TestHandleLoginNotImplemented() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "login_resp", 2, ErrorCode::INTERNAL_ERROR);
  assert(resp["message"].get<std::string>() == "login not implemented");
}

void TestHandleRegisterDbQueryFailedWithoutDbConfig() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"register","seq":3,"token":"","data":{"username":"alice","password":"123456","nickname":"Alice"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "register_resp", 3, ErrorCode::DB_QUERY_FAILED);
  assert(resp["message"].get<std::string>() == "query user failed");
}

void TestHandleLogoutNotImplemented() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"logout","seq":4,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "logout_resp", 4, ErrorCode::INTERNAL_ERROR);
  assert(resp["message"].get<std::string>() == "logout not implemented");
}

void TestHandleUnknownMessageType() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"chat","seq":5,"token":"","data":{}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "chat_resp", 5, ErrorCode::UNKNOWN_MESSAGE_TYPE);
  assert(resp["message"].get<std::string>() == "Unknown message type: chat");
}

void TestHandleLoginMissingPassword() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"login","seq":6,"token":"","data":{"username":"alice"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "login_resp", 6, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("password") != std::string::npos);
}

void TestHandleRegisterMissingUsername() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"register","seq":7,"token":"","data":{"password":"123456"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "register_resp", 7, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("username") != std::string::npos);
}

void TestHandleInvalidDataFieldType() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"login","seq":8,"token":"","data":"bad"})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("data") != std::string::npos);
}

void TestHandleInvalidSeqType() {
  MessageHandler handler;
  const std::string request =
      R"({"msg_type":"login","seq":"9","token":"","data":{"username":"alice","password":"123456"}})";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("seq") != std::string::npos);
}

void TestHandleInvalidJson() {
  MessageHandler handler;
  const std::string request = R"({"msg_type":"login","seq":10,"data":)";

  const nlohmann::json resp = ParseResponse(handler.handle(request));
  ExpectCommonEnvelope(resp, "_resp", 0, ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("JSON") != std::string::npos);
}

}  // namespace

int main() {
  TestHandleHeartbeatSuccess();
  TestHandleLoginNotImplemented();
  TestHandleRegisterDbQueryFailedWithoutDbConfig();
  TestHandleLogoutNotImplemented();
  TestHandleUnknownMessageType();
  TestHandleLoginMissingPassword();
  TestHandleRegisterMissingUsername();
  TestHandleInvalidDataFieldType();
  TestHandleInvalidSeqType();
  TestHandleInvalidJson();

  std::cout << "[PASS] message handler tests passed\n";
  return 0;
}
