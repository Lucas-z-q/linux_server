#ifndef LINUX_SERVER_INCLUDE_SERVICE_GROUP_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_GROUP_SERVICE_H_

#include <optional>
#include <string>
#include <vector>

#include "common/error_code.h"
#include "db/group_repository.h"
#include "db/message_repository.h"
#include "db/user_repository.h"
#include "protocol/group_messages.h"
#include "server/session_manager.h"

namespace chat {

struct CreateGroupResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::optional<GroupRecord> group;
    std::vector<UserId> member_ids;
};

struct AddGroupMemberResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::optional<GroupMemberRecord> member;
};

struct SendGroupMessageResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::string group_id;
    std::string conversation_id;
    UserId from_user_id = 0;
    std::string from_username;
    std::string content;
    Timestamp server_time = 0;
    std::vector<GroupMessageDelivery> messages;
};

class GroupService {
   public:
    GroupService(ISessionManager& session_manager, IGroupRepository& group_repository,
                 IMessageRepository& message_repository, IUserRepository& user_repository);

    CreateGroupResult createGroup(ConnectionId conn_id, const CreateGroupRequest& req);
    AddGroupMemberResult addGroupMember(ConnectionId conn_id, const AddGroupMemberRequest& req);
    SendGroupMessageResult sendGroupMessage(ConnectionId conn_id, const SendGroupMessageRequest& req);

   private:
    ISessionManager& session_manager_;
    IGroupRepository& group_repository_;
    IMessageRepository& message_repository_;
    IUserRepository& user_repository_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_GROUP_SERVICE_H_
