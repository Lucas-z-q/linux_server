#include "service/user_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>

namespace
{

class FakeUserRepository : public chat::IUserRepository
{
 public:
  chat::FindUserResult find_by_username_result;
  chat::FindUserResult find_by_id_result;
  chat::CreateUserResult create_user_result;

  std::string last_username;
  std::string last_password_hash;
  std::string last_nickname;

  chat::FindUserResult findByUsername(const std::string &username) override
  {
    last_username = username;
    return find_by_username_result;
  }

  chat::FindUserResult findById(chat::UserId user_id) override
  {
    (void)user_id;
    return find_by_id_result;
  }

  chat::CreateUserResult createUser(const std::string &username,
                                    const std::string &password_hash,
                                    const std::string &nickname) override
  {
    last_username = username;
    last_password_hash = password_hash;
    last_nickname = nickname;
    return create_user_result;
  }
};

chat::RegisterRequest MakeRegisterRequest()
{
  chat::RegisterRequest req;
  req.username = "alice";
  req.password = "123456";
  req.nickname = "Alice";
  return req;
}

void TestRegisterReturnsInvalidParamForEmptyUsername()
{
  FakeUserRepository repo;
  chat::UserService service(repo);

  chat::RegisterRequest req = MakeRegisterRequest();
  req.username.clear();

  const chat::RegisterResult result = service.registerUser(req);
  assert(result.code == chat::ErrorCode::INVALID_PARAM);
  assert(result.message == "username is empty");
}

void TestRegisterReturnsUserAlreadyExistsWhenUserFound()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kOk;
  chat::UserRecord record;
  record.id = 7;
  record.username = "alice";
  repo.find_by_username_result.user = record;
  chat::UserService service(repo);

  const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
  assert(result.code == chat::ErrorCode::USER_ALREADY_EXISTS);
  assert(result.message == "username already exists");
}

void TestRegisterReturnsDbQueryFailedWhenLookupFails()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kQueryFailed;
  chat::UserService service(repo);

  const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
  assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
  assert(result.message == "query user failed");
}

void TestRegisterReturnsDbInsertFailedWhenCreateFails()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
  repo.create_user_result.status = chat::RepositoryStatus::kInsertFailed;
  chat::UserService service(repo);

  const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
  assert(result.code == chat::ErrorCode::DB_INSERT_FAILED);
  assert(result.message == "create user failed");
}

void TestRegisterReturnsUserAlreadyExistsWhenCreateSeesDuplicate()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
  repo.create_user_result.status = chat::RepositoryStatus::kDuplicate;
  chat::UserService service(repo);

  const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
  assert(result.code == chat::ErrorCode::USER_ALREADY_EXISTS);
  assert(result.message == "username already exists");
}

void TestRegisterSuccessReturnsUserIdAndHashesPassword()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
  repo.create_user_result.status = chat::RepositoryStatus::kOk;
  repo.create_user_result.user_id = 10001;
  chat::UserService service(repo);

  const chat::RegisterRequest req = MakeRegisterRequest();
  const chat::RegisterResult result = service.registerUser(req);

  assert(result.code == chat::ErrorCode::OK);
  assert(result.message == "register success");
  assert(result.data.user_id == 10001);
  assert(repo.last_username == "alice");
  assert(repo.last_nickname == "Alice");
  assert(repo.last_password_hash != req.password);
  assert(!repo.last_password_hash.empty());
}

} // namespace

int main()
{
  TestRegisterReturnsInvalidParamForEmptyUsername();
  TestRegisterReturnsUserAlreadyExistsWhenUserFound();
  TestRegisterReturnsDbQueryFailedWhenLookupFails();
  TestRegisterReturnsDbInsertFailedWhenCreateFails();
  TestRegisterReturnsUserAlreadyExistsWhenCreateSeesDuplicate();
  TestRegisterSuccessReturnsUserIdAndHashesPassword();
  std::cout << "[PASS] user service tests passed\n";
  return 0;
}
