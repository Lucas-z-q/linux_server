#ifndef LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_

#include <string>

#include "common/error_code.h"
#include "common/types.h"
#include "db/message_repository.h"
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

    // 服务端生成的消息唯一标识。
    std::string message_id;

    // 会话 ID。
    std::string conversation_id;

    // 消息状态。
    int32_t status = 0;

    // 消息创建时间戳。
    Timestamp created_at = 0;
};

// 表示拉取离线消息的业务执行结果。
struct PullOfflineMessagesResult {
    // 错误码。
    ErrorCode code = ErrorCode::OK;

    // 错误或状态信息。
    std::string message;

    // 离线消息列表。
    std::vector<OfflineMessage> messages;

    // 是否还有更多。
    bool has_more = false;
};

// 负责处理单聊等即时通讯核心业务逻辑。
class ChatService {
   public:
    // 构造函数，注入会话管理器和消息仓储。
    ChatService(ISessionManager& session_manager, IMessageRepository& message_repository);

    // 发送一条单聊消息。
    SendMessageResult sendMessage(ConnectionId from_conn_id, const SendMessageRequest& req);

    // 拉取离线消息。
    PullOfflineMessagesResult pullOfflineMessages(ConnectionId from_conn_id, const PullOfflineMessagesRequest& req);

   private:
    ISessionManager& session_manager_;
    IMessageRepository& message_repository_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
