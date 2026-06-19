#ifndef LINUX_SERVER_INCLUDE_SERVICE_FRIEND_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_FRIEND_SERVICE_H_

#include <optional>
#include <string>
#include <vector>

#include "common/error_code.h"
#include "common/types.h"
#include "db/friend_repository.h"
#include "db/user_repository.h"
#include "protocol/friend_messages.h"
#include "server/session_manager.h"

namespace chat {

struct FriendshipActionResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::optional<FriendshipRecord> friendship;
    UserId friend_user_id = 0;
    bool deleted = false;
};

struct ListFriendsResult {
    ErrorCode code = ErrorCode::OK;
    std::string message;
    std::vector<FriendshipListItem> friends;
};

class FriendService {
   public:
    FriendService(ISessionManager& session_manager, IFriendRepository& friend_repository,
                  IUserRepository& user_repository);

    FriendshipActionResult addFriend(ConnectionId conn_id, const AddFriendRequest& req);
    FriendshipActionResult acceptFriend(ConnectionId conn_id, const AcceptFriendRequest& req);
    FriendshipActionResult deleteFriend(ConnectionId conn_id, const DeleteFriendRequest& req);
    ListFriendsResult listFriends(ConnectionId conn_id);

   private:
    ISessionManager& session_manager_;
    IFriendRepository& friend_repository_;
    IUserRepository& user_repository_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_FRIEND_SERVICE_H_
