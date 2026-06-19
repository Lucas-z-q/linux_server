#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_FRIEND_MESSAGES_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_FRIEND_MESSAGES_H_

#include <string>
#include <vector>

#include "common/types.h"
#include "model/friendship_record.h"

namespace chat {

struct AddFriendRequest {
    UserId target_user_id = 0;
};

struct AcceptFriendRequest {
    UserId requester_user_id = 0;
};

struct DeleteFriendRequest {
    UserId friend_user_id = 0;
};

struct FriendshipActionResponseData {
    UserId requester_id = 0;
    UserId addressee_id = 0;
    UserId friend_user_id = 0;
    FriendshipStatus status = FriendshipStatus::kPending;
    std::string created_at;
    std::string updated_at;
};

struct DeleteFriendResponseData {
    UserId friend_user_id = 0;
    bool deleted = false;
};

struct ListFriendsResponseData {
    std::vector<FriendshipListItem> friends;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_FRIEND_MESSAGES_H_
