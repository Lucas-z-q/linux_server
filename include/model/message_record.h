#ifndef LINUX_SERVER_INCLUDE_MODEL_MESSAGE_RECORD_H_
#define LINUX_SERVER_INCLUDE_MODEL_MESSAGE_RECORD_H_

#include <cstdint>
#include <string>

#include "common/types.h"

namespace chat {

// 对应 messages.status 的稳定领域状态。
// 数据库当前存储值：0=stored, 1=delivered, 2=read。
// delivered 表示服务端已将在线 push 或离线拉取响应放入 I/O 发送队列，不等价于客户端已读。
enum class MessageStatus : int32_t {
    kStored = 0,
    kDelivered = 1,
    kRead = 2,
};

inline int32_t ToStorageMessageStatus(MessageStatus status) { return static_cast<int32_t>(status); }

inline int32_t ToProtocolMessageStatus(MessageStatus status) { return static_cast<int32_t>(status); }

inline bool ParseMessageStatus(int32_t raw_status, MessageStatus* status) {
    if (status == nullptr) {
        return false;
    }
    switch (raw_status) {
        case 0:
            *status = MessageStatus::kStored;
            return true;
        case 1:
            *status = MessageStatus::kDelivered;
            return true;
        case 2:
            *status = MessageStatus::kRead;
            return true;
        default:
            return false;
    }
}

// 状态只允许向前推进；delivered/read 自身重复提交视为幂等成功。
inline bool CanTransitionMessageStatus(MessageStatus from, MessageStatus to) {
    if (from == to) {
        return from == MessageStatus::kDelivered || from == MessageStatus::kRead;
    }
    if (from == MessageStatus::kStored) {
        return to == MessageStatus::kDelivered || to == MessageStatus::kRead;
    }
    return from == MessageStatus::kDelivered && to == MessageStatus::kRead;
}

// messages 表在服务端内部使用的数据模型。
struct MessageRecord {
    std::string id;
    std::string conversation_id;
    std::string client_msg_id;
    UserId from_user_id = 0;
    UserId to_user_id = 0;
    std::string content;
    MessageStatus status = MessageStatus::kStored;
    Timestamp created_at = 0;
    Timestamp delivered_at = 0;
    Timestamp read_at = 0;
};

// conversations 表在服务端内部使用的数据模型。
struct ConversationRecord {
    std::string id;
    std::string type;
    std::string single_chat_key;
    std::string created_at;
    std::string updated_at;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_MODEL_MESSAGE_RECORD_H_
