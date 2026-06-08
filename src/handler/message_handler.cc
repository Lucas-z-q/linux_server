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

HandleResult MessageHandler::handle(const std::string &raw_request, const RequestContext &context) {
    const ConnectionId conn_id = context.conn_id;
    Message msg;
    std::string err;
    if (!codec_.decodeMessage(raw_request, msg, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    // 任意合法请求都可维持已认证连接的在线状态。
    user_service_.refreshPresence(conn_id);

    if (msg.msg_type == "register") {
        return handleRegister(msg, context.peer_ip);
    } else if (msg.msg_type == "login") {
        return handleLogin(msg, conn_id, context.peer_ip);
    } else if (msg.msg_type == "logout") {
        return handleLogout(msg, conn_id);
    } else if (msg.msg_type == "heartbeat") {
        return handleHeartbeat(msg);
    } else if (msg.msg_type == "whoami") {
        return handleWhoAmI(msg, conn_id);
    } else if (msg.msg_type == "send_message") {
        return handleSendMessage(msg, conn_id);
    } else if (msg.msg_type == "pull_offline_messages") {
        return handlePullOfflineMessages(msg, conn_id);
    } else {
        return handleUnknown(msg);
    }
}

void MessageHandler::onConnectionClosed(chat::ConnectionId conn_id) { user_service_.clearSession(conn_id); }

HandleResult MessageHandler::handleRegister(const Message &msg, const std::string &identity) {
    RegisterRequest req;
    std::string err;
    if (!codec_.parseRegisterRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    RegisterResult result = user_service_.registerUser(req, identity);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::RATE_LIMITED) {
        resp.data["retry_after_seconds"] = result.retry_after_seconds;
    }

    if (result.code == ErrorCode::OK) {
        RegisterResponseData data;
        data.user_id = result.data.user_id;
        codec_.fillRegisterResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleLogin(const Message &msg, chat::ConnectionId conn_id, const std::string &identity) {
    LoginRequest req;
    std::string err;
    if (!codec_.parseLoginRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    LoginResult result = user_service_.login(req, conn_id, identity);

    Response resp;
    resp.msg_type = msg.msg_type + "_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::RATE_LIMITED) {
        resp.data["retry_after_seconds"] = result.retry_after_seconds;
    }

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
    LogoutResult result = user_service_.logout(conn_id);

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
    const WhoAmIResult result = user_service_.whoami(conn_id);

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

HandleResult MessageHandler::handleSendMessage(const Message &msg, chat::ConnectionId conn_id) {
    SendMessageRequest req;
    std::string err;
    if (!codec_.parseSendMessageRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    SendMessageResult result = chat_service_.sendMessage(conn_id, req);

    Response ack;
    ack.msg_type = "send_message_resp";
    ack.seq = msg.seq;
    ack.code = result.code;
    ack.message = result.message;
    if (result.code == ErrorCode::RATE_LIMITED) {
        ack.data["retry_after_seconds"] = result.retry_after_seconds;
    }

    HandleResult handle_result;
    if (result.code == ErrorCode::OK) {
        SendMessageAckData ack_data;
        ack_data.message_id = result.message_id;
        ack_data.conversation_id = result.conversation_id;
        ack_data.to_user_id = result.to_user_id;
        ack_data.status = result.status;
        ack_data.created_at = result.created_at;
        codec_.fillSendMessageAck(ack, ack_data);

        if (result.to_conn_id != 0 || !result.remote_server_id.empty()) {
            Response push;
            push.msg_type = "message_push";
            push.seq = 0;
            push.code = ErrorCode::OK;
            push.message = "new message";

            MessagePushData push_data;
            push_data.message_id = result.message_id;
            push_data.conversation_id = result.conversation_id;
            push_data.from_user_id = result.from_user_id;
            push_data.from_username = result.from_username;
            push_data.to_user_id = result.to_user_id;
            push_data.content = result.content;
            push_data.created_at = result.created_at;
            push_data.server_time = result.server_time;
            codec_.fillMessagePush(push, push_data);

            const std::string payload = codec_.encodeResponse(push);
            if (result.to_conn_id != 0) {
                handle_result.pushes.push_back({result.to_conn_id, result.to_user_id, payload, result.message_id});
            } else {
                chat_service_.publishRemotePush(result, payload);
            }
        }
    }

    handle_result.response = codec_.encodeResponse(ack);
    return handle_result;
}

HandleResult MessageHandler::handlePullOfflineMessages(const Message &msg, chat::ConnectionId conn_id) {
    PullOfflineMessagesRequest req;
    std::string err;
    if (!codec_.parsePullOfflineMessagesRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    PullOfflineMessagesResult result = chat_service_.pullOfflineMessages(conn_id, req);

    Response resp;
    resp.msg_type = "pull_offline_messages_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    HandleResult handle_result;
    if (result.code == ErrorCode::OK) {
        PullOfflineMessagesResponseData resp_data;
        resp_data.messages = result.messages;
        resp_data.has_more = result.has_more;
        codec_.fillPullOfflineMessagesResponse(resp, resp_data);

        if (!result.messages.empty()) {
            handle_result.delivered_user_id = result.messages[0].to_user_id;
            handle_result.delivered_message_ids.reserve(result.messages.size());
            for (const auto &m : result.messages) {
                handle_result.delivered_message_ids.push_back(m.message_id);
            }
        }
    }

    handle_result.response = codec_.encodeResponse(resp);
    return handle_result;
}
void MessageHandler::onMessagesDelivered(chat::UserId user_id, const std::vector<std::string> &message_ids) {
    chat_service_.markMessagesDelivered(user_id, message_ids);
}

}  // namespace chat
