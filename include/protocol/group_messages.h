#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_GROUP_MESSAGES_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_GROUP_MESSAGES_H_

#include <string>
#include <vector>

#include "common/types.h"
#include "model/group_record.h"

namespace chat {

struct CreateGroupRequest {
    std::string name;
    std::vector<UserId> member_ids;
};

struct AddGroupMemberRequest {
    std::string group_id;
    UserId user_id = 0;
};

struct SendGroupMessageRequest {
    std::string group_id;
    std::string client_msg_id;
    std::string content;
};

struct CreateGroupResponseData {
    GroupRecord group;
    std::vector<UserId> member_ids;
};

struct AddGroupMemberResponseData {
    GroupMemberRecord member;
};

struct GroupMessageDelivery {
    std::string message_id;
    std::string group_id;
    std::string conversation_id;
    int64_t sequence = 0;
    UserId to_user_id = 0;
    ConnectionId target_conn_id = 0;
    int32_t status = 0;
    Timestamp created_at = 0;
};

struct SendGroupMessageResponseData {
    std::string group_id;
    std::string conversation_id;
    std::vector<GroupMessageDelivery> messages;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_GROUP_MESSAGES_H_
