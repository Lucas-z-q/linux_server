#include "service/friend_service.h"

#include <optional>
#include <string>

namespace chat {
namespace {

ErrorCode MapRepositoryError(RepositoryStatus status) {
    switch (status) {
        case RepositoryStatus::kInsertFailed:
        case RepositoryStatus::kDuplicate:
            return ErrorCode::DB_INSERT_FAILED;
        case RepositoryStatus::kQueryFailed:
        case RepositoryStatus::kConnectionUnavailable:
        case RepositoryStatus::kBorrowTimeout:
        case RepositoryStatus::kNotFound:
            return ErrorCode::DB_QUERY_FAILED;
        default:
            return ErrorCode::INTERNAL_ERROR;
    }
}

FriendshipActionResult MakeActionError(ErrorCode code, const std::string& message) {
    FriendshipActionResult result;
    result.code = code;
    result.message = message;
    return result;
}

ListFriendsResult MakeListError(ErrorCode code, const std::string& message) {
    ListFriendsResult result;
    result.code = code;
    result.message = message;
    return result;
}

std::optional<ConnectionSession> GetAuthenticatedSession(ISessionManager& session_manager, ConnectionId conn_id) {
    std::optional<ConnectionSession> session = session_manager.GetSession(conn_id);
    if (!session.has_value() || !session->authenticated) {
        return std::nullopt;
    }
    return session;
}

std::optional<FriendshipActionResult> ValidateUserExists(IUserRepository& user_repository, UserId user_id,
                                                         const std::string& missing_message,
                                                         const std::string& query_failed_message) {
    const FindUserResult user = user_repository.findById(user_id);
    if (user.status == RepositoryStatus::kNotFound || (user.status == RepositoryStatus::kOk && !user.user)) {
        return MakeActionError(ErrorCode::USER_NOT_FOUND, missing_message);
    }
    if (user.status != RepositoryStatus::kOk) {
        return MakeActionError(MapRepositoryError(user.status), query_failed_message);
    }
    return std::nullopt;
}

FriendshipActionResult ExistingFriendshipError(const FriendshipRecord& friendship) {
    if (friendship.status == FriendshipStatus::kAccepted) {
        return MakeActionError(ErrorCode::FRIENDSHIP_ALREADY_EXISTS, "friendship already exists");
    }
    if (friendship.status == FriendshipStatus::kBlocked) {
        return MakeActionError(ErrorCode::FRIENDSHIP_BLOCKED, "friendship is blocked");
    }
    return MakeActionError(ErrorCode::FRIEND_REQUEST_ALREADY_EXISTS, "friend request already exists");
}

}  // namespace

FriendService::FriendService(ISessionManager& session_manager, IFriendRepository& friend_repository,
                             IUserRepository& user_repository)
    : session_manager_(session_manager), friend_repository_(friend_repository), user_repository_(user_repository) {}

FriendshipActionResult FriendService::addFriend(ConnectionId conn_id, const AddFriendRequest& req) {
    if (req.target_user_id <= 0) {
        return MakeActionError(ErrorCode::INVALID_PARAM, "target_user_id must be greater than 0");
    }
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return MakeActionError(ErrorCode::NOT_LOGGED_IN, "User not logged in");
    }
    if (req.target_user_id == session->user_id) {
        return MakeActionError(ErrorCode::CANNOT_SEND_TO_SELF, "cannot add yourself as friend");
    }
    if (const auto error = ValidateUserExists(user_repository_, req.target_user_id, "target user not found",
                                              "query target user failed")) {
        return *error;
    }

    const FindFriendshipResult existing = friend_repository_.findFriendship(session->user_id, req.target_user_id);
    if (existing.status == RepositoryStatus::kOk && existing.friendship) {
        return ExistingFriendshipError(*existing.friendship);
    }
    if (existing.status != RepositoryStatus::kNotFound) {
        return MakeActionError(MapRepositoryError(existing.status), "query friendship failed");
    }

    const CreateFriendshipResult created = friend_repository_.createFriendRequest(session->user_id, req.target_user_id);
    if (created.status == RepositoryStatus::kDuplicate) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_ALREADY_EXISTS, "friend request already exists");
    }
    if (created.status != RepositoryStatus::kOk || !created.friendship) {
        return MakeActionError(MapRepositoryError(created.status), "create friend request failed");
    }

    FriendshipActionResult result;
    result.code = ErrorCode::OK;
    result.message = "friend request sent";
    result.friendship = created.friendship;
    result.friend_user_id = req.target_user_id;
    return result;
}

FriendshipActionResult FriendService::acceptFriend(ConnectionId conn_id, const AcceptFriendRequest& req) {
    if (req.requester_user_id <= 0) {
        return MakeActionError(ErrorCode::INVALID_PARAM, "requester_user_id must be greater than 0");
    }
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return MakeActionError(ErrorCode::NOT_LOGGED_IN, "User not logged in");
    }
    if (req.requester_user_id == session->user_id) {
        return MakeActionError(ErrorCode::CANNOT_SEND_TO_SELF, "cannot accept your own friend request");
    }
    if (const auto error = ValidateUserExists(user_repository_, req.requester_user_id, "requester user not found",
                                              "query requester user failed")) {
        return *error;
    }

    const FindFriendshipResult existing = friend_repository_.findFriendship(req.requester_user_id, session->user_id);
    if (existing.status == RepositoryStatus::kNotFound) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_NOT_FOUND, "friend request not found");
    }
    if (existing.status != RepositoryStatus::kOk) {
        return MakeActionError(MapRepositoryError(existing.status), "query friendship failed");
    }
    if (!existing.friendship) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_NOT_FOUND, "friend request not found");
    }
    if (existing.friendship->status == FriendshipStatus::kAccepted) {
        return MakeActionError(ErrorCode::FRIENDSHIP_ALREADY_EXISTS, "friendship already exists");
    }
    if (existing.friendship->status == FriendshipStatus::kBlocked) {
        return MakeActionError(ErrorCode::FRIENDSHIP_BLOCKED, "friendship is blocked");
    }
    if (existing.friendship->requester_id != req.requester_user_id ||
        existing.friendship->addressee_id != session->user_id) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_NOT_FOUND, "friend request not found");
    }

    const UpdateFriendshipResult accepted =
        friend_repository_.acceptFriendRequest(req.requester_user_id, session->user_id);
    if (accepted.status == RepositoryStatus::kNotFound) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_NOT_FOUND, "friend request not found");
    }
    if (accepted.status != RepositoryStatus::kOk) {
        return MakeActionError(MapRepositoryError(accepted.status), "accept friend request failed");
    }
    if (!accepted.friendship) {
        return MakeActionError(ErrorCode::FRIEND_REQUEST_NOT_FOUND, "friend request not found");
    }

    FriendshipActionResult result;
    result.code = ErrorCode::OK;
    result.message = "friend request accepted";
    result.friendship = accepted.friendship;
    result.friend_user_id = req.requester_user_id;
    return result;
}

FriendshipActionResult FriendService::deleteFriend(ConnectionId conn_id, const DeleteFriendRequest& req) {
    if (req.friend_user_id <= 0) {
        return MakeActionError(ErrorCode::INVALID_PARAM, "friend_user_id must be greater than 0");
    }
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return MakeActionError(ErrorCode::NOT_LOGGED_IN, "User not logged in");
    }
    if (req.friend_user_id == session->user_id) {
        return MakeActionError(ErrorCode::CANNOT_SEND_TO_SELF, "cannot delete yourself as friend");
    }

    const DeleteFriendshipResult deleted = friend_repository_.deleteFriendship(session->user_id, req.friend_user_id);
    if (deleted.status == RepositoryStatus::kNotFound) {
        return MakeActionError(ErrorCode::FRIENDSHIP_NOT_FOUND, "friendship not found");
    }
    if (deleted.status != RepositoryStatus::kOk) {
        return MakeActionError(MapRepositoryError(deleted.status), "delete friendship failed");
    }
    if (deleted.affected_rows == 0) {
        return MakeActionError(ErrorCode::FRIENDSHIP_NOT_FOUND, "friendship not found");
    }

    FriendshipActionResult result;
    result.code = ErrorCode::OK;
    result.message = "friendship deleted";
    result.friend_user_id = req.friend_user_id;
    result.deleted = true;
    return result;
}

ListFriendsResult FriendService::listFriends(ConnectionId conn_id) {
    const std::optional<ConnectionSession> session = GetAuthenticatedSession(session_manager_, conn_id);
    if (!session) {
        return MakeListError(ErrorCode::NOT_LOGGED_IN, "User not logged in");
    }

    const ListFriendshipsResult list = friend_repository_.listFriendships(session->user_id);
    if (list.status != RepositoryStatus::kOk) {
        return MakeListError(MapRepositoryError(list.status), "list friends failed");
    }

    ListFriendsResult result;
    result.code = ErrorCode::OK;
    result.message = "Success";
    result.friends = list.friends;
    return result;
}

}  // namespace chat
