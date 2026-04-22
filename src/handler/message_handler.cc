#include "handler/message_handler.h"

#include <chrono>
#include <utility>

#include "common/error_code.h"
#include "protocol/protocol_helper.h"
#include "protocol/auth_messages.h"

namespace chat
{

  namespace
  {

    Response MakeInvalidParamResponse(const Message &msg,
                                      const std::string &message)
    {
      Response resp;
      resp.msg_type = msg.msg_type + "_resp";
      resp.seq = msg.seq;
      resp.code = ErrorCode::INVALID_PARAM;
      resp.message = message;
      return resp;
    }

  } // namespace

  std::string MessageHandler::handle(const std::string &raw_request, chat::ConnectionId conn_id)
  {
    Message msg;
    std::string err;
    if (!codec_.decodeMessage(raw_request, msg, err))
    {
      Response resp = MakeInvalidParamResponse(msg, err);
      return codec_.encodeResponse(resp);
    }

    if (msg.msg_type == "register")
    {
      Response resp = handleRegister(msg);
      return codec_.encodeResponse(resp);
    }
    else if (msg.msg_type == "login")
    {
      Response resp = handleLogin(msg, conn_id);
      return codec_.encodeResponse(resp);
    }
    else if (msg.msg_type == "logout")
    {
      Response resp = handleLogout(msg, conn_id);
      return codec_.encodeResponse(resp);
    }
    else if (msg.msg_type == "heartbeat")
    {
      Response resp = handleHeartbeat(msg);
      return codec_.encodeResponse(resp);
    }
    else if (msg.msg_type == "whoami")
    {
      Response resp = handleWhoAmI(msg, conn_id);
      return codec_.encodeResponse(resp);
    }
    else
    {
      Response resp = handleUnknown(msg);
      return codec_.encodeResponse(resp);
    }
  }

  void MessageHandler::onConnectionClosed(chat::ConnectionId conn_id)
  {
    user_service_->clearSession(conn_id);
  }

  Response MessageHandler::handleRegister(const Message &msg)
  {
    RegisterRequest req;
    std::string err;
    if (!codec_.parseRegisterRequest(msg, req, err))
    {
      return MakeInvalidParamResponse(msg, err);
    }

    RegisterResult result = user_service_->registerUser(req);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    if (result.code == ErrorCode::OK)
    {
      RegisterResponseData data;
      data.user_id = result.data.user_id;
      codec_.fillRegisterResponse(resp, data);
    }
    return resp;
  }

  Response MessageHandler::handleLogin(const Message &msg, chat::ConnectionId conn_id)
  {
    LoginRequest req;
    std::string err;
    if (!codec_.parseLoginRequest(msg, req, err))
    {
      return MakeInvalidParamResponse(msg, err);
    }

    LoginResult result = user_service_->login(req, conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    if (result.code == ErrorCode::OK)

    {
      LoginResponseData data;
      data.user_id = result.data.user_id;
      data.nickname = result.data.nickname;
      data.token = result.data.token;
      codec_.fillLoginResponse(resp, data);
    }
    return resp;
  }

  Response MessageHandler::handleLogout(const Message &msg, chat::ConnectionId conn_id)
  {
    LogoutResult result = user_service_->logout(conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    return resp;
  }

  Response MessageHandler::handleHeartbeat(const Message &msg)
  {
    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = ErrorCode::OK;
    resp.message = "Heartbeat received";

    chat::HeartbeatResponseData data;
    data.server_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    codec_.fillHeartbeatResponse(resp, data);
    return resp;
  }

  Response MessageHandler::handleWhoAmI(const Message &msg, chat::ConnectionId conn_id)
  {
    const WhoAmIResult result = user_service_->whoami(conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK)
    {
      resp.data["user_id"] = result.data.user_id;
      resp.data["username"] = result.data.username;
      resp.data["token"] = result.data.token;
    }
    return resp;
  }

  Response MessageHandler::handleUnknown(const Message &msg)
  {
    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = ErrorCode::UNKNOWN_MESSAGE_TYPE;
    resp.message = "Unknown message type: " + msg.msg_type;
    return resp;
  }
} // namespace chat
