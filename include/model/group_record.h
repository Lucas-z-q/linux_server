#ifndef LINUX_SERVER_INCLUDE_MODEL_GROUP_RECORD_H_
#define LINUX_SERVER_INCLUDE_MODEL_GROUP_RECORD_H_

#include <string>

#include "common/types.h"

namespace chat {

struct GroupRecord {
    std::string id;
    std::string name;
    UserId owner_id = 0;
    std::string conversation_id;
    std::string created_at;
    std::string updated_at;
};

struct GroupMemberRecord {
    std::string group_id;
    UserId user_id = 0;
    std::string role = "member";
    std::string joined_at;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_MODEL_GROUP_RECORD_H_
