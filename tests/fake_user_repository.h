#ifndef LINUX_SERVER_TESTS_FAKE_USER_REPOSITORY_H_
#define LINUX_SERVER_TESTS_FAKE_USER_REPOSITORY_H_

#include <string>
#include <unordered_map>

#include "db/user_repository.h"
#include "model/user_record.h"

class FakeUserRepository : public chat::IUserRepository {
   public:
    FakeUserRepository() {
        AddUser(1, "user1");
        AddUser(2, "user2");
        AddUser(3, "user3");
        AddUser(10001, "alice");
        AddUser(10002, "bob");
        AddUser(10003, "charlie");
    }

    chat::FindUserResult find_by_username_result;
    chat::FindUserResult find_by_id_result;
    chat::CreateUserResult create_user_result;
    int find_by_username_calls = 0;
    int find_by_id_calls = 0;
    int create_user_calls = 0;

    chat::UserRecord AddUser(chat::UserId user_id, const std::string& username,
                             const std::string& password_hash = "password_hash", const std::string& nickname = "") {
        chat::UserRecord record;
        record.id = user_id;
        record.username = username;
        record.password_hash = password_hash;
        record.nickname = nickname.empty() ? username : nickname;
        record.status = 1;
        record.created_at = "2026-01-01 00:00:00";
        record.updated_at = "2026-01-01 00:00:00";
        users_by_id_[user_id] = record;
        users_by_name_[username] = record;
        if (user_id >= next_user_id_) {
            next_user_id_ = user_id + 1;
        }
        return record;
    }

    chat::FindUserResult findByUsername(const std::string& username) override {
        ++find_by_username_calls;
        if (find_by_username_result.status != chat::RepositoryStatus::kNotFound ||
            find_by_username_result.user.has_value()) {
            return find_by_username_result;
        }
        const auto it = users_by_name_.find(username);
        if (it == users_by_name_.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .user = it->second};
    }

    chat::FindUserResult findById(chat::UserId user_id) override {
        ++find_by_id_calls;
        if (find_by_id_result.status != chat::RepositoryStatus::kNotFound || find_by_id_result.user.has_value()) {
            return find_by_id_result;
        }
        const auto it = users_by_id_.find(user_id);
        if (it == users_by_id_.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .user = it->second};
    }

    chat::CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                      const std::string& nickname) override {
        ++create_user_calls;
        if (create_user_result.status != chat::RepositoryStatus::kInsertFailed || create_user_result.user_id != 0) {
            return create_user_result;
        }
        if (users_by_name_.find(username) != users_by_name_.end()) {
            return {.status = chat::RepositoryStatus::kDuplicate};
        }
        const chat::UserId user_id = next_user_id_++;
        AddUser(user_id, username, password_hash, nickname);
        return {.status = chat::RepositoryStatus::kOk, .user_id = user_id};
    }

   private:
    std::unordered_map<chat::UserId, chat::UserRecord> users_by_id_;
    std::unordered_map<std::string, chat::UserRecord> users_by_name_;
    chat::UserId next_user_id_ = 1;
};

#endif  // LINUX_SERVER_TESTS_FAKE_USER_REPOSITORY_H_
