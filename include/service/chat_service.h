#ifndef LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_

#include <string>

#include "cache/message_dedup_cache.h"
#include "cache/redis_rate_limiter.h"
#include "common/error_code.h"
#include "common/types.h"
#include "db/message_repository.h"
#include "db/user_repository.h"
#include "protocol/chat_messages.h"
#include "server/redis_session_store.h"
#include "server/session_manager.h"
#include "stream/remote_push.h"

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
    std::vector<ConnectionId> to_conn_ids;
    std::vector<ConnectionId> sender_sync_conn_ids;

    // 远端在线时由 Handler 构造协议 payload 后发布到目标 server。
    std::string remote_server_id;
    ConnectionId remote_conn_id = 0;

    // 消息文本内容。
    std::string content;

    // 服务端处理消息时的系统时间戳。
    Timestamp server_time = 0;

    // 服务端生成的消息唯一标识。
    std::string message_id;

    // 会话 ID。
    std::string conversation_id;

    // 会话内单调递增序号。
    int64_t sequence = 0;

    // 消息状态。
    int32_t status = 0;

    // 消息创建时间戳。
    Timestamp created_at = 0;
    std::uint32_t retry_after_seconds = 0;
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

struct MessageStateUpdateResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::vector<std::string> message_ids;
    int32_t affected_rows = 0;
};

// 负责处理单聊等即时通讯核心业务逻辑。
class ChatService {
   public:
    // 构造函数，注入会话管理器、消息仓储和用户仓储。
    ChatService(ISessionManager& session_manager, IMessageRepository& message_repository,
                IUserRepository& user_repository, IRateLimiter* rate_limiter = nullptr,
                IMessageDedupCache* dedup_cache = nullptr, RedisConfig config = {},
                IGlobalSessionStore* global_session_store = nullptr,
                IRemotePushPublisher* remote_push_publisher = nullptr);

    // 发送一条单聊消息。
    SendMessageResult sendMessage(ConnectionId from_conn_id, const SendMessageRequest& req);

    // Stream 发布失败不改变发送成功语义，消息仍可通过离线拉取恢复。
    bool publishRemotePush(const SendMessageResult& result, const std::string& payload);

    // 拉取离线消息。
    PullOfflineMessagesResult pullOfflineMessages(ConnectionId from_conn_id, const PullOfflineMessagesRequest& req);

    MessageStateUpdateResult acknowledgeMessages(ConnectionId conn_id, const MessageAckRequest& req);

    MessageStateUpdateResult markMessagesRead(ConnectionId conn_id, const MarkMessageReadRequest& req);

    // I/O 线程在推送/响应成功入队后回调，批量标记消息已投递。
    void markMessagesDelivered(UserId user_id, const std::vector<std::string>& message_ids);

   private:
    ISessionManager& session_manager_;
    IMessageRepository& message_repository_;
    IUserRepository& user_repository_;
    IRateLimiter* rate_limiter_;
    IMessageDedupCache* dedup_cache_;
    RedisConfig redis_config_;
    IGlobalSessionStore* global_session_store_;
    IRemotePushPublisher* remote_push_publisher_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_CHAT_SERVICE_H_
