#include "handler/message_handler.h"

#include <chrono>
#include <utility>

#include "common/error_code.h"
#include "protocol/auth_messages.h"
#include "protocol/protocol_helper.h"

namespace chat {

namespace {

Response MakeInvalidParamResponse(const Message &msg, const std::string &message) {
    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = ErrorCode::INVALID_PARAM;
    resp.message = message;
    return resp;
}

}  // namespace

HandleResult MessageHandler::handle(const std::string &raw_request, chat::ConnectionId conn_id) {
    Message msg;
    std::string err;
    if (!codec_.decodeMessage(raw_request, msg, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    if (msg.msg_type == "register") {
        return handleRegister(msg);
    } else if (msg.msg_type == "login") {
        return handleLogin(msg, conn_id);
    } else if (msg.msg_type == "logout") {
        return handleLogout(msg, conn_id);
    } else if (msg.msg_type == "heartbeat") {
        return handleHeartbeat(msg);
    } else if (msg.msg_type == "whoami") {
        return handleWhoAmI(msg, conn_id);
    } else {
        return handleUnknown(msg);
    }
}

void MessageHandler::onConnectionClosed(chat::ConnectionId conn_id) { user_service_->clearSession(conn_id); }

HandleResult MessageHandler::handleRegister(const Message &msg) {
    RegisterRequest req;
    std::string err;
    if (!codec_.parseRegisterRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    RegisterResult result = user_service_->registerUser(req);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    if (result.code == ErrorCode::OK) {
        RegisterResponseData data;
        data.user_id = result.data.user_id;
        codec_.fillRegisterResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleLogin(const Message &msg, chat::ConnectionId conn_id) {
    LoginRequest req;
    std::string err;
    if (!codec_.parseLoginRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    LoginResult result = user_service_->login(req, conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    HandleResult res;
    if (result.code == ErrorCode::OK) {
        LoginResponseData data;
        data.user_id = result.data.user_id;
        data.nickname = result.data.nickname;
        data.token = result.data.token;
        codec_.fillLoginResponse(resp, data);

        res.session_action = SessionAction::BIND;
        res.pending_session = result.session;
    }
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleLogout(const Message &msg, chat::ConnectionId conn_id) {
    LogoutResult result = user_service_->logout(conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    HandleResult res;
    if (result.code == ErrorCode::OK) {
        res.session_action = SessionAction::UNBIND;
    }
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleHeartbeat(const Message &msg) {
    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = ErrorCode::OK;
    resp.message = "Heartbeat received";

    chat::HeartbeatResponseData data;
    data.server_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    codec_.fillHeartbeatResponse(resp, data);

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleWhoAmI(const Message &msg, chat::ConnectionId conn_id) {
    const WhoAmIResult result = user_service_->whoami(conn_id);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK) {
        resp.data["user_id"] = result.data.user_id;
        resp.data["username"] = result.data.username;
        resp.data["token"] = result.data.token;
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleUnknown(const Message &msg) {
    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = ErrorCode::UNKNOWN_MESSAGE_TYPE;
    resp.message = "Unknown message type: " + msg.msg_type;

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}
}  // namespace chat
