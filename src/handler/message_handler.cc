#include "handler/message_handler.h"

#include <chrono>
#include <utility>

#include "common/error_code.h"
#include "protocol/protocol_helper.h"

namespace chat {

namespace {

Response MakeInvalidParamResponse(const Message& msg,
                                  const std::string& message) {
  return protocol::makeErrorResponse(msg, ErrorCode::INVALID_PARAM, message);
}

}  // namespace

std::string MessageHandler::handle(const std::string& raw_request) {
  Message msg;
  std::string err;
  if (!codec_.decodeMessage(raw_request, msg, err)) {
    Response resp;
    resp.msg_type = "error_resp";
    resp.code = ErrorCode::INVALID_JSON;
    resp.message = err.empty() ? "invalid json" : err;
    resp.data = nlohmann::json::object();
    return codec_.encodeResponse(resp);
  }

  Response resp;
  if (msg.msg_type == "register") {
    resp = handleRegister(msg);
  } else if (msg.msg_type == "login") {
    resp = handleLogin(msg);
  } else if (msg.msg_type == "logout") {
    resp = handleLogout(msg);
  } else if (msg.msg_type == "heartbeat") {
    resp = handleHeartbeat(msg);
  } else {
    resp = protocol::makeErrorResponse(msg, ErrorCode::UNKNOWN_MESSAGE_TYPE,
                                       "unknown message type");
  }

  return codec_.encodeResponse(resp);
}

Response MessageHandler::handleRegister(const Message& msg) {
  RegisterRequest req;
  std::string err;
  if (!codec_.parseRegisterRequest(msg, req, err)) {
    return MakeInvalidParamResponse(msg, err);
  }

  const RegisterResult result = user_service_.registerUser(req);
  Response resp = protocol::makeErrorResponse(msg, result.code, result.message);
  if (result.code == ErrorCode::OK) {
    resp = protocol::makeSuccessResponse(msg);
    resp.message = result.message;
    codec_.fillRegisterResponse(resp, result.data);
  }
  return resp;
}

Response MessageHandler::handleLogin(const Message& msg) {
  LoginRequest req;
  std::string err;
  if (!codec_.parseLoginRequest(msg, req, err)) {
    return MakeInvalidParamResponse(msg, err);
  }

  const LoginResult result = user_service_.login(req, 0);
  Response resp = protocol::makeErrorResponse(msg, result.code, result.message);
  if (result.code == ErrorCode::OK) {
    resp = protocol::makeSuccessResponse(msg);
    resp.message = result.message;
    codec_.fillLoginResponse(resp, result.data);
  }
  return resp;
}

Response MessageHandler::handleLogout(const Message& msg) {
  const LogoutResult result = user_service_.logout(0);
  Response resp = protocol::makeErrorResponse(msg, result.code, result.message);
  if (result.code == ErrorCode::OK) {
    resp = protocol::makeSuccessResponse(msg);
    resp.message = result.message;
    resp.data = nlohmann::json::object();
  }
  return resp;
}

Response MessageHandler::handleHeartbeat(const Message& msg) {
  Response resp = protocol::makeSuccessResponse(msg);
  resp.message = "pong";

  HeartbeatResponseData data;
  data.server_time = static_cast<Timestamp>(
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  codec_.fillHeartbeatResponse(resp, data);
  return resp;
}

}  // namespace chat
