#include "service/chat_service.h"

#include <chrono>

namespace chat {

ChatService::ChatService(ISessionManager& session_manager)
    : session_manager_(session_manager) {}

SendMessageResult ChatService::sendMessage(ConnectionId from_conn_id,
                                          const SendMessageRequest& req) {
    // 1. 获取发送方会话
    auto from_session_opt = session_manager_.GetSession(from_conn_id);

    // 2. 如果没有 session，返回 user not logged in
    if (!from_session_opt.has_value() || !from_session_opt->authenticated) {
        SendMessageResult result;
        result.code = ErrorCode::NOT_LOGGED_IN;
        result.message = "User not logged in";
        return result;
    }

    // 3. 如果 req.receiver_id == 当前用户 id，返回 CANNOT_SEND_TO_SELF
    if (req.receiver_id == from_session_opt->user_id) {
        SendMessageResult result;
        result.code = ErrorCode::CANNOT_SEND_TO_SELF;
        result.message = "Cannot send message to yourself";
        return result;
    }

    // 4. 获取接收方连接 ID
    auto target_conn_id_opt = session_manager_.GetConnectionId(req.receiver_id);

    // 5. 如果目标用户不在线，返回 USER_NOT_ONLINE
    if (!target_conn_id_opt.has_value()) {
        SendMessageResult result;
        result.code = ErrorCode::USER_NOT_ONLINE;
        result.message = "Target user not online";
        return result;
    }

    // 6. 返回发送方、接收方、目标连接、消息内容等信息
    SendMessageResult result;
    result.code = ErrorCode::OK;
    result.message = "Success";
    result.from_user_id = from_session_opt->user_id;
    result.from_username = from_session_opt->username;
    result.to_user_id = req.receiver_id;
    result.to_conn_id = target_conn_id_opt.value();
    result.content = req.content;
    result.server_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return result;
}

}  // namespace chat
