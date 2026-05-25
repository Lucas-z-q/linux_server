#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_

#include <string>

#include "common/types.h"

// 本文件定义聊天相关协议消息的业务字段模型。
// 这些结构体只描述 data 字段内容，不包含通用信封字段。
//
// TODO(lzq): 为每个请求结构补充字段合法性约束说明。
// TODO(lzq): 增加群聊消息、文件消息等扩展结构。

namespace chat {

// 发送消息请求中的业务字段。
struct SendMessageRequest {
    // 接收者用户 ID。
    UserId receiver_id = 0;

    // 消息文本内容。
    std::string content;
};

// 发送消息成功后返回给发送者的确认数据。
struct SendMessageAckData {
    // 接收者用户 ID。
    UserId receiver_id = 0;
};

// 服务端推送给接收者的消息数据。
struct MessagePushData {
    UserId from_user_id = 0;
    std::string from_username;
    std::string content;
    Timestamp server_time = 0;
};
}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_CHAT_MESSAGES_H_