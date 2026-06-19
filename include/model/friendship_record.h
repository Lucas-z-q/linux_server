#ifndef LINUX_SERVER_INCLUDE_MODEL_FRIENDSHIP_RECORD_H_
#define LINUX_SERVER_INCLUDE_MODEL_FRIENDSHIP_RECORD_H_

#include <cstdint>
#include <string>

#include "common/types.h"

namespace chat {

// 对应 friendships.status 的稳定领域状态。
// 数据库存储值：pending、accepted、blocked。
enum class FriendshipStatus : int32_t {
    kPending = 0,
    kAccepted = 1,
    kBlocked = 2,
};

inline std::string ToStorageFriendshipStatus(FriendshipStatus status) {
    switch (status) {
        case FriendshipStatus::kPending:
            return "pending";
        case FriendshipStatus::kAccepted:
            return "accepted";
        case FriendshipStatus::kBlocked:
            return "blocked";
    }
    return "pending";
}

inline std::string ToProtocolFriendshipStatus(FriendshipStatus status) { return ToStorageFriendshipStatus(status); }

inline bool ParseFriendshipStatus(const std::string& raw_status, FriendshipStatus* status) {
    if (status == nullptr) {
        return false;
    }
    if (raw_status == "pending") {
        *status = FriendshipStatus::kPending;
        return true;
    }
    if (raw_status == "accepted") {
        *status = FriendshipStatus::kAccepted;
        return true;
    }
    if (raw_status == "blocked") {
        *status = FriendshipStatus::kBlocked;
        return true;
    }
    return false;
}

struct FriendshipRecord {
    std::int64_t id = 0;
    UserId requester_id = 0;
    UserId addressee_id = 0;
    FriendshipStatus status = FriendshipStatus::kPending;
    std::string created_at;
    std::string updated_at;
};

struct FriendshipListItem {
    UserId user_id = 0;
    std::string username;
    std::string nickname;
    FriendshipStatus status = FriendshipStatus::kPending;
    std::string direction;
    std::string created_at;
    std::string updated_at;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_MODEL_FRIENDSHIP_RECORD_H_
