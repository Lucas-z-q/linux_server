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
  const auto by_name = repo.findByUsername("alice");
  const auto by_id = repo.findById(1);

  assert(!by_name.has_value());
  assert(!by_id.has_value());
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
  chat::UserId user_id = 42;
  const bool ok =
      repo.createUser("alice", "hash_xxx", "Alice", user_id);

  assert(!ok);
  assert(user_id == 0);
}

} // namespace

int main()
{
  TestFindReturnsEmptyWhenConfigIncomplete();
  TestCreateReturnsFalseWhenConfigIncomplete();
  std::cout << "[PASS] user repository tests passed\n";
  return 0;
}
