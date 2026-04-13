#include "db/user_repository.h"

namespace chat {

std::optional<UserRecord> UserRepository::findByUsername(
    const std::string& username) {
  (void)username;
  return std::nullopt;
}

std::optional<UserRecord> UserRepository::findById(UserId user_id) {
  (void)user_id;
  return std::nullopt;
}

bool UserRepository::createUser(const std::string& username,
                                const std::string& password_hash,
                                const std::string& nickname, UserId& user_id) {
  (void)username;
  (void)password_hash;
  (void)nickname;
  user_id = 0;
  return false;
}

}  // namespace chat
