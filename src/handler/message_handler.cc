#include "handler/message_handler.h"

#include <chrono>
#include <utility>

#include "common/error_code.h"
#include "protocol/auth_messages.h"

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

bool IsProtectedMessage(const std::string &message_type) {
    return message_type == "logout" || message_type == "whoami" || message_type == "send_message" ||
           message_type == "pull_offline_messages" || message_type == "message_ack" ||
           message_type == "mark_message_read" || message_type == "add_friend" || message_type == "accept_friend" ||
           message_type == "delete_friend" || message_type == "list_friends" || message_type == "create_group" ||
           message_type == "add_group_member" || message_type == "send_group_message";
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

    if (msg.msg_type == "resume_session") {
        return handleResumeSession(msg);
    }
    if (IsProtectedMessage(msg.msg_type) && !msg.token.empty() &&
        !user_service_.requestTokenMatches(conn_id, msg.token)) {
        Response resp;
        resp.msg_type = msg.msg_type + "_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INVALID_CREDENTIALS;
        resp.message = "request token does not match connection session";
        HandleResult result;
        result.response = codec_.encodeResponse(resp);
        return result;
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
    } else if (msg.msg_type == "message_ack") {
        return handleMessageAck(msg, conn_id);
    } else if (msg.msg_type == "mark_message_read") {
        return handleMarkMessageRead(msg, conn_id);
    } else if (msg.msg_type == "add_friend") {
        return handleAddFriend(msg, conn_id);
    } else if (msg.msg_type == "accept_friend") {
        return handleAcceptFriend(msg, conn_id);
    } else if (msg.msg_type == "delete_friend") {
        return handleDeleteFriend(msg, conn_id);
    } else if (msg.msg_type == "list_friends") {
        return handleListFriends(msg, conn_id);
    } else if (msg.msg_type == "create_group") {
        return handleCreateGroup(msg, conn_id);
    } else if (msg.msg_type == "add_group_member") {
        return handleAddGroupMember(msg, conn_id);
    } else if (msg.msg_type == "send_group_message") {
        return handleSendGroupMessage(msg, conn_id);
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

HandleResult MessageHandler::handleResumeSession(const Message &msg) {
    const ResumeSessionResult resume = user_service_.resumeSession(msg.token);
    Response resp;
    resp.msg_type = "resume_session_resp";
    resp.seq = msg.seq;
    resp.code = resume.code;
    resp.message = resume.message;

    HandleResult result;
    if (resume.code == ErrorCode::OK) {
        resp.data["user_id"] = resume.session.user_id;
        resp.data["username"] = resume.session.username;
        result.session_action = SessionAction::BIND;
        result.pending_session = resume.session;
    }
    result.response = codec_.encodeResponse(resp);
    return result;
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
        ack_data.sequence = result.sequence;
        ack_data.to_user_id = result.to_user_id;
        ack_data.status = result.status;
        ack_data.created_at = result.created_at;
        codec_.fillSendMessageAck(ack, ack_data);

        Response push;
        push.msg_type = "message_push";
        push.seq = 0;
        push.code = ErrorCode::OK;
        push.message = "new message";

        MessagePushData push_data;
        push_data.message_id = result.message_id;
        push_data.conversation_id = result.conversation_id;
        push_data.sequence = result.sequence;
        push_data.from_user_id = result.from_user_id;
        push_data.from_username = result.from_username;
        push_data.to_user_id = result.to_user_id;
        push_data.content = result.content;
        push_data.created_at = result.created_at;
        push_data.server_time = result.server_time;
        codec_.fillMessagePush(push, push_data);

        const std::string payload = codec_.encodeResponse(push);
        std::vector<ConnectionId> target_conn_ids = result.to_conn_ids;
        if (target_conn_ids.empty() && result.to_conn_id != 0) {
            target_conn_ids.push_back(result.to_conn_id);
        }
        for (const ConnectionId target_conn_id : target_conn_ids) {
            handle_result.pushes.push_back({target_conn_id, result.to_user_id, payload, ""});
        }
        if (target_conn_ids.empty() && !result.remote_server_id.empty()) {
            chat_service_.publishRemotePush(result, payload);
        }

        if (!result.sender_sync_conn_ids.empty()) {
            Response sync_push = push;
            sync_push.msg_type = "message_sync_push";
            sync_push.message = "message synced";
            const std::string sync_payload = codec_.encodeResponse(sync_push);
            for (const ConnectionId sync_conn_id : result.sender_sync_conn_ids) {
                handle_result.pushes.push_back({sync_conn_id, result.from_user_id, sync_payload, ""});
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
    }

    handle_result.response = codec_.encodeResponse(resp);
    return handle_result;
}
void MessageHandler::onMessagesDelivered(chat::UserId user_id, const std::vector<std::string> &message_ids) {
    chat_service_.markMessagesDelivered(user_id, message_ids);
}

HandleResult MessageHandler::handleMessageAck(const Message &msg, chat::ConnectionId conn_id) {
    MessageAckRequest req;
    std::string err;
    if (!codec_.parseMessageAckRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const MessageStateUpdateResult result = chat_service_.acknowledgeMessages(conn_id, req);
    Response resp;
    resp.msg_type = "message_ack_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK) {
        MessageStateUpdateResponseData data;
        data.message_ids = result.message_ids;
        data.affected_rows = result.affected_rows;
        codec_.fillMessageStateUpdateResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleMarkMessageRead(const Message &msg, chat::ConnectionId conn_id) {
    MarkMessageReadRequest req;
    std::string err;
    if (!codec_.parseMarkMessageReadRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const MessageStateUpdateResult result = chat_service_.markMessagesRead(conn_id, req);
    Response resp;
    resp.msg_type = "mark_message_read_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK) {
        MessageStateUpdateResponseData data;
        data.message_ids = result.message_ids;
        data.affected_rows = result.affected_rows;
        codec_.fillMessageStateUpdateResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleAddFriend(const Message &msg, chat::ConnectionId conn_id) {
    if (friend_service_ == nullptr) {
        Response resp;
        resp.msg_type = "add_friend_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "friend service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    AddFriendRequest req;
    std::string err;
    if (!codec_.parseAddFriendRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const FriendshipActionResult result = friend_service_->addFriend(conn_id, req);
    Response resp;
    resp.msg_type = "add_friend_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK && result.friendship) {
        FriendshipActionResponseData data;
        data.requester_id = result.friendship->requester_id;
        data.addressee_id = result.friendship->addressee_id;
        data.friend_user_id = result.friend_user_id;
        data.status = result.friendship->status;
        data.created_at = result.friendship->created_at;
        data.updated_at = result.friendship->updated_at;
        codec_.fillFriendshipActionResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleAcceptFriend(const Message &msg, chat::ConnectionId conn_id) {
    if (friend_service_ == nullptr) {
        Response resp;
        resp.msg_type = "accept_friend_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "friend service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    AcceptFriendRequest req;
    std::string err;
    if (!codec_.parseAcceptFriendRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const FriendshipActionResult result = friend_service_->acceptFriend(conn_id, req);
    Response resp;
    resp.msg_type = "accept_friend_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK && result.friendship) {
        FriendshipActionResponseData data;
        data.requester_id = result.friendship->requester_id;
        data.addressee_id = result.friendship->addressee_id;
        data.friend_user_id = result.friend_user_id;
        data.status = result.friendship->status;
        data.created_at = result.friendship->created_at;
        data.updated_at = result.friendship->updated_at;
        codec_.fillFriendshipActionResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleDeleteFriend(const Message &msg, chat::ConnectionId conn_id) {
    if (friend_service_ == nullptr) {
        Response resp;
        resp.msg_type = "delete_friend_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "friend service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    DeleteFriendRequest req;
    std::string err;
    if (!codec_.parseDeleteFriendRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const FriendshipActionResult result = friend_service_->deleteFriend(conn_id, req);
    Response resp;
    resp.msg_type = "delete_friend_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK) {
        DeleteFriendResponseData data;
        data.friend_user_id = result.friend_user_id;
        data.deleted = result.deleted;
        codec_.fillDeleteFriendResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleListFriends(const Message &msg, chat::ConnectionId conn_id) {
    if (friend_service_ == nullptr) {
        Response resp;
        resp.msg_type = "list_friends_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "friend service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const ListFriendsResult result = friend_service_->listFriends(conn_id);
    Response resp;
    resp.msg_type = "list_friends_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK) {
        ListFriendsResponseData data;
        data.friends = result.friends;
        codec_.fillListFriendsResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleCreateGroup(const Message &msg, chat::ConnectionId conn_id) {
    if (group_service_ == nullptr) {
        Response resp;
        resp.msg_type = "create_group_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "group service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    CreateGroupRequest req;
    std::string err;
    if (!codec_.parseCreateGroupRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const CreateGroupResult result = group_service_->createGroup(conn_id, req);
    Response resp;
    resp.msg_type = "create_group_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK && result.group) {
        CreateGroupResponseData data;
        data.group = *result.group;
        data.member_ids = result.member_ids;
        codec_.fillCreateGroupResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleAddGroupMember(const Message &msg, chat::ConnectionId conn_id) {
    if (group_service_ == nullptr) {
        Response resp;
        resp.msg_type = "add_group_member_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "group service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    AddGroupMemberRequest req;
    std::string err;
    if (!codec_.parseAddGroupMemberRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const AddGroupMemberResult result = group_service_->addGroupMember(conn_id, req);
    Response resp;
    resp.msg_type = "add_group_member_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;
    if (result.code == ErrorCode::OK && result.member) {
        AddGroupMemberResponseData data;
        data.member = *result.member;
        codec_.fillAddGroupMemberResponse(resp, data);
    }

    HandleResult res;
    res.response = codec_.encodeResponse(resp);
    return res;
}

HandleResult MessageHandler::handleSendGroupMessage(const Message &msg, chat::ConnectionId conn_id) {
    if (group_service_ == nullptr) {
        Response resp;
        resp.msg_type = "send_group_message_resp";
        resp.seq = msg.seq;
        resp.code = ErrorCode::INTERNAL_ERROR;
        resp.message = "group service unavailable";
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    SendGroupMessageRequest req;
    std::string err;
    if (!codec_.parseSendGroupMessageRequest(msg, req, err)) {
        Response resp = MakeInvalidParamResponse(msg, err);
        HandleResult res;
        res.response = codec_.encodeResponse(resp);
        return res;
    }

    const SendGroupMessageResult result = group_service_->sendGroupMessage(conn_id, req);
    Response resp;
    resp.msg_type = "send_group_message_resp";
    resp.seq = msg.seq;
    resp.code = result.code;
    resp.message = result.message;

    HandleResult handle_result;
    if (result.code == ErrorCode::OK) {
        SendGroupMessageResponseData data;
        data.group_id = result.group_id;
        data.conversation_id = result.conversation_id;
        data.messages = result.messages;
        codec_.fillSendGroupMessageResponse(resp, data);

        for (const GroupMessageDelivery &delivery : result.messages) {
            if (delivery.target_conn_id == 0) {
                continue;
            }

            Response push;
            push.msg_type = "group_message_push";
            push.seq = 0;
            push.code = ErrorCode::OK;
            push.message = "new group message";
            push.data["message_id"] = delivery.message_id;
            push.data["group_id"] = delivery.group_id;
            push.data["conversation_id"] = delivery.conversation_id;
            push.data["sequence"] = delivery.sequence;
            push.data["from_user_id"] = result.from_user_id;
            push.data["from_username"] = result.from_username;
            push.data["to_user_id"] = delivery.to_user_id;
            push.data["content"] = result.content;
            push.data["created_at"] = delivery.created_at;
            push.data["server_time"] = result.server_time;
            handle_result.pushes.push_back(
                {delivery.target_conn_id, delivery.to_user_id, codec_.encodeResponse(push), ""});
        }
    }

    handle_result.response = codec_.encodeResponse(resp);
    return handle_result;
}

}  // namespace chat
