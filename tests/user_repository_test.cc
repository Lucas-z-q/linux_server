#include "db/user_repository.h"

#include <cassert>
#include <iostream>

namespace
{

void TestFindReturnsEmptyWhenConfigIncomplete()
{
  chat::DbConfig config;
  config.host = "127.0.0.1";
  config.port = 3307;
  config.username = "";
  config.password = "";
  config.database = "";

  chat::UserRepository repo(config);
  const chat::FindUserResult by_name = repo.findByUsername("alice");
  const chat::FindUserResult by_id = repo.findById(1);

  assert(by_name.status == chat::RepositoryStatus::kQueryFailed);
  assert(!by_name.user.has_value());
  assert(by_id.status == chat::RepositoryStatus::kQueryFailed);
  assert(!by_id.user.has_value());
}

void TestCreateReturnsFalseWhenConfigIncomplete()
{
  chat::DbConfig config;
  config.host = "127.0.0.1";
  config.port = 3307;
  config.username = "";
  config.password = "";
  config.database = "";

  chat::UserRepository repo(config);
  const chat::CreateUserResult result =
      repo.createUser("alice", "hash_xxx", "Alice");

  assert(result.status == chat::RepositoryStatus::kInsertFailed);
  assert(result.user_id == 0);
}

} // namespace

int main()
{
  TestFindReturnsEmptyWhenConfigIncomplete();
  TestCreateReturnsFalseWhenConfigIncomplete();
  std::cout << "[PASS] user repository tests passed\n";
  return 0;
}
