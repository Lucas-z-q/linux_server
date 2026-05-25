#ifndef LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_

#include <string>

#include "common/error_code.h"
#include "common/types.h"
#include "protocol/chat_messages.h"
#include "server/session_manager.h"

namespace chat {

// 表示发送消息的业务执行结果。
struct SendMessageResult {
    // 错误码。
    ErrorCode code = ErrorCode::OK;

    // 错误或状态信息。
    std::string message;

    // 发送方用户 ID。
    UserId from_user_id = 0;

    // 发送方用户名。
    std::string from_username;

    // 接收方用户 ID。
    UserId to_user_id = 0;

    // 接收方当前所在的连接 ID。
    ConnectionId to_conn_id = 0;

    // 消息文本内容。
    std::string content;

    // 服务端处理消息时的系统时间戳。
    Timestamp server_time = 0;
};

// 负责处理单聊等即时通讯核心业务逻辑。
class ChatService {
public:
    // 构造函数，注入会话管理器。
    explicit ChatService(ISessionManager& session_manager);

    // 发送一条单聊消息。
    SendMessageResult sendMessage(ConnectionId from_conn_id,
                                  const SendMessageRequest& req);

private:
    ISessionManager& session_manager_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
