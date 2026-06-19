#ifndef LINUX_SERVER_INCLUDE_DB_FRIEND_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_DB_FRIEND_REPOSITORY_H_

#include <optional>
#include <vector>

#include "common/types.h"
#include "db/user_repository.h"
#include "model/friendship_record.h"

namespace chat {

struct FindFriendshipResult {
    RepositoryStatus status = RepositoryStatus::kNotFound;
    std::optional<FriendshipRecord> friendship;
};

struct CreateFriendshipResult {
    RepositoryStatus status = RepositoryStatus::kInsertFailed;
    std::optional<FriendshipRecord> friendship;
    bool created = false;
};

struct UpdateFriendshipResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::optional<FriendshipRecord> friendship;
};

struct DeleteFriendshipResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    int32_t affected_rows = 0;
};

struct ListFriendshipsResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::vector<FriendshipListItem> friends;
};

class IFriendRepository {
   public:
    virtual ~IFriendRepository() = default;

    virtual FindFriendshipResult findFriendship(UserId user_a, UserId user_b) = 0;
    virtual CreateFriendshipResult createFriendRequest(UserId requester_id, UserId addressee_id) = 0;
    virtual UpdateFriendshipResult acceptFriendRequest(UserId requester_id, UserId addressee_id) = 0;
    virtual DeleteFriendshipResult deleteFriendship(UserId user_a, UserId user_b) = 0;
    virtual ListFriendshipsResult listFriendships(UserId user_id) = 0;
};

class DbPool;

class FriendRepository : public IFriendRepository {
   public:
    explicit FriendRepository(DbPool* pool);

    FindFriendshipResult findFriendship(UserId user_a, UserId user_b) override;
    CreateFriendshipResult createFriendRequest(UserId requester_id, UserId addressee_id) override;
    UpdateFriendshipResult acceptFriendRequest(UserId requester_id, UserId addressee_id) override;
    DeleteFriendshipResult deleteFriendship(UserId user_a, UserId user_b) override;
    ListFriendshipsResult listFriendships(UserId user_id) override;

   private:
    DbPool* pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_FRIEND_REPOSITORY_H_
