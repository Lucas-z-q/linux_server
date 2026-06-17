#ifndef LINUX_SERVER_TESTS_FAKE_FRIEND_REPOSITORY_H_
#define LINUX_SERVER_TESTS_FAKE_FRIEND_REPOSITORY_H_

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "db/friend_repository.h"

class FakeFriendRepository : public chat::IFriendRepository {
   public:
    chat::RepositoryStatus find_status = chat::RepositoryStatus::kNotFound;
    chat::RepositoryStatus create_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus accept_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus delete_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus list_status = chat::RepositoryStatus::kOk;
    std::vector<chat::FriendshipRecord> records;
    std::vector<chat::FriendshipListItem> list_items;
    int create_calls = 0;
    int accept_calls = 0;
    int delete_calls = 0;
    int list_calls = 0;

    chat::FindFriendshipResult findFriendship(chat::UserId user_a, chat::UserId user_b) override {
        if (find_status != chat::RepositoryStatus::kOk && find_status != chat::RepositoryStatus::kNotFound) {
            return {.status = find_status};
        }
        const auto it = FindByPair(user_a, user_b);
        if (it == records.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .friendship = *it};
    }

    chat::CreateFriendshipResult createFriendRequest(chat::UserId requester_id, chat::UserId addressee_id) override {
        ++create_calls;
        if (create_status != chat::RepositoryStatus::kOk) {
            return {.status = create_status};
        }
        if (FindByPair(requester_id, addressee_id) != records.end()) {
            return {.status = chat::RepositoryStatus::kDuplicate};
        }

        chat::FriendshipRecord record;
        record.id = static_cast<int64_t>(records.size() + 1);
        record.requester_id = requester_id;
        record.addressee_id = addressee_id;
        record.status = chat::FriendshipStatus::kPending;
        record.created_at = "2026-06-16 00:00:00";
        record.updated_at = "2026-06-16 00:00:00";
        records.push_back(record);
        return {.status = chat::RepositoryStatus::kOk, .friendship = record, .created = true};
    }

    chat::UpdateFriendshipResult acceptFriendRequest(chat::UserId requester_id, chat::UserId addressee_id) override {
        ++accept_calls;
        if (accept_status != chat::RepositoryStatus::kOk) {
            return {.status = accept_status};
        }
        const auto it = FindByPair(requester_id, addressee_id);
        if (it == records.end() || it->requester_id != requester_id || it->addressee_id != addressee_id ||
            it->status != chat::FriendshipStatus::kPending) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        it->status = chat::FriendshipStatus::kAccepted;
        it->updated_at = "2026-06-16 00:00:01";
        return {.status = chat::RepositoryStatus::kOk, .friendship = *it};
    }

    chat::DeleteFriendshipResult deleteFriendship(chat::UserId user_a, chat::UserId user_b) override {
        ++delete_calls;
        if (delete_status != chat::RepositoryStatus::kOk) {
            return {.status = delete_status};
        }
        const auto it = FindByPair(user_a, user_b);
        if (it == records.end()) {
            return {.status = chat::RepositoryStatus::kNotFound, .affected_rows = 0};
        }
        records.erase(it);
        return {.status = chat::RepositoryStatus::kOk, .affected_rows = 1};
    }

    chat::ListFriendshipsResult listFriendships(chat::UserId user_id) override {
        ++list_calls;
        if (list_status != chat::RepositoryStatus::kOk) {
            return {.status = list_status};
        }
        if (!list_items.empty()) {
            return {.status = chat::RepositoryStatus::kOk, .friends = list_items};
        }

        chat::ListFriendshipsResult result;
        result.status = chat::RepositoryStatus::kOk;
        for (const auto& record : records) {
            if (record.requester_id != user_id && record.addressee_id != user_id) {
                continue;
            }
            chat::FriendshipListItem item;
            item.user_id = record.requester_id == user_id ? record.addressee_id : record.requester_id;
            item.username = item.user_id == 1 ? "user1" : "user" + std::to_string(item.user_id);
            item.nickname = item.username;
            item.status = record.status;
            item.direction = record.requester_id == user_id ? "outgoing" : "incoming";
            item.created_at = record.created_at;
            item.updated_at = record.updated_at;
            result.friends.push_back(item);
        }
        return result;
    }

   private:
    std::vector<chat::FriendshipRecord>::iterator FindByPair(chat::UserId user_a, chat::UserId user_b) {
        const chat::UserId low = std::min(user_a, user_b);
        const chat::UserId high = std::max(user_a, user_b);
        return std::find_if(records.begin(), records.end(), [low, high](const chat::FriendshipRecord& record) {
            return std::min(record.requester_id, record.addressee_id) == low &&
                   std::max(record.requester_id, record.addressee_id) == high;
        });
    }
};

#endif  // LINUX_SERVER_TESTS_FAKE_FRIEND_REPOSITORY_H_
