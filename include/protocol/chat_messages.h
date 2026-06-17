#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_

#include <string>
#include <vector>

#include "common/types.h"

// 本文件定义聊天相关协议消息的业务字段模型。
// 这些结构体只描述 data 字段内容，不包含通用信封字段。
//
// TODO(lzq): 为每个请求结构补充字段合法性约束说明。
// TODO(lzq): 增加群聊消息、文件消息等扩展结构。

namespace chat {

// 发送消息请求中的业务字段。
struct SendMessageRequest {
    // 客户端生成的消息唯一标识。
    std::string client_msg_id;

    // 接收者用户 ID。
    UserId to_user_id = 0;

    // 消息文本内容。
    std::string content;
};

// 发送消息成功后返回给发送者的确认数据。
struct SendMessageAckData {
    // 服务端生成的消息唯一标识。
    std::string message_id;

    // 会话 ID。
    std::string conversation_id;

    // 会话内单调递增序号。
    int64_t sequence = 0;

    // 接收者用户 ID。
    UserId to_user_id = 0;

    // 消息状态。
    int32_t status = 0;

    // 消息创建时间戳。
    Timestamp created_at = 0;
};

// 服务端推送给接收者的消息数据。
struct MessagePushData {
    // 服务端生成的消息唯一标识。
    std::string message_id;

    // 会话 ID。
    std::string conversation_id;

    // 会话内单调递增序号。
    int64_t sequence = 0;

    // 发送方用户 ID。
    UserId from_user_id = 0;

    // 发送方用户名。
    std::string from_username;

    // 接收方用户 ID。
    UserId to_user_id = 0;

    // 消息文本内容。
    std::string content;

    // 消息创建时间戳。
    Timestamp created_at = 0;

    // 服务端处理消息时的系统时间戳（保留兼容）。
    Timestamp server_time = 0;
};

// 单条离线消息结构。
struct OfflineMessage {
    std::string message_id;
    std::string conversation_id;
    int64_t sequence = 0;
    UserId from_user_id = 0;
    UserId to_user_id = 0;
    std::string content;
    Timestamp created_at = 0;
    int32_t status = 0;
};

// 拉取离线消息请求。
struct PullOfflineMessagesRequest {
    // 限制返回的消息条数。
    int32_t limit = 0;

    // 可选：在此消息 ID 之前的消息（分页）。
    std::string before_message_id;

    // 可选：在此消息 ID 之后的消息（分页）。
    std::string since_message_id;
};

// 拉取离线消息响应数据。
struct PullOfflineMessagesResponseData {
    // 离线消息列表。
    std::vector<OfflineMessage> messages;

    // 是否还有更多离线消息。
    bool has_more = false;
};

struct MessageAckRequest {
    std::vector<std::string> message_ids;
};

struct MarkMessageReadRequest {
    std::vector<std::string> message_ids;
};

struct MessageStateUpdateResponseData {
    std::vector<std::string> message_ids;
    int32_t affected_rows = 0;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_
