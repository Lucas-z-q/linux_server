#include "service/user_service.h"

#include <cassert>
#include <functional>
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

class FakeSessionManager : public chat::ISessionManager
{
 public:
  bool bind_result = true;
  bool bind_called = false;
  bool clear_called = false;
  chat::ConnectionId last_connection_id = 0;
  chat::ConnectionSession last_session;
  std::optional<chat::ConnectionSession> session_for_connection;

  bool BindSession(chat::ConnectionId connection_id,
                   const chat::ConnectionSession &session) override
  {
    bind_called = true;
    last_connection_id = connection_id;
    last_session = session;
    if (bind_result)
    {
      session_for_connection = session;
    }
    return bind_result;
  }

  std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override
  {
    if (session_for_connection.has_value() &&
        session_for_connection->user_id == user_id)
    {
      return last_connection_id;
    }
    return std::nullopt;
  }

  std::optional<chat::ConnectionSession> GetSession(
      chat::ConnectionId connection_id) override
  {
    if (session_for_connection.has_value() && connection_id == last_connection_id)
    {
      return session_for_connection;
    }
    return std::nullopt;
  }

  void ClearSession(chat::ConnectionId connection_id) override
  {
    if (session_for_connection.has_value() && connection_id == last_connection_id)
    {
      clear_called = true;
      session_for_connection.reset();
    }
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

chat::LoginRequest MakeLoginRequest()
{
  chat::LoginRequest req;
  req.username = "alice";
  req.password = "123456";
  return req;
}

std::string HashPasswordForTest(const std::string &password)
{
  return std::to_string(std::hash<std::string>{}(password));
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

void TestLoginReturnsInvalidParamForEmptyUsername()
{
  FakeUserRepository repo;
  chat::UserService service(repo);

  chat::LoginRequest req = MakeLoginRequest();
  req.username.clear();

  const chat::LoginResult result = service.login(req, 0);
  assert(result.code == chat::ErrorCode::INVALID_PARAM);
  assert(result.message == "username is empty");
}

void TestLoginReturnsInvalidParamForEmptyPassword()
{
  FakeUserRepository repo;
  chat::UserService service(repo);

  chat::LoginRequest req = MakeLoginRequest();
  req.password.clear();

  const chat::LoginResult result = service.login(req, 0);
  assert(result.code == chat::ErrorCode::INVALID_PARAM);
  assert(result.message == "password is empty");
}

void TestLoginReturnsDbQueryFailedWhenLookupFails()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kQueryFailed;
  chat::UserService service(repo);

  const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
  assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
  assert(result.message == "query user failed");
}

void TestLoginReturnsUserNotFoundWhenUserDoesNotExist()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
  chat::UserService service(repo);

  const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
  assert(result.code == chat::ErrorCode::INVALID_CREDENTIALS);
  assert(result.message == "invalid username or password");
}

void TestLoginReturnsInternalErrorWhenRepositoryResultIsIncomplete()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kOk;
  repo.find_by_username_result.user.reset();
  chat::UserService service(repo);

  const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
  assert(result.code == chat::ErrorCode::INTERNAL_ERROR);
  assert(result.message == "unexpected repository result");
}

void TestLoginReturnsWrongPasswordWhenHashDoesNotMatch()
{
  FakeUserRepository repo;
  repo.find_by_username_result.status = chat::RepositoryStatus::kOk;
  chat::UserRecord record;
  record.id = 10001;
  record.username = "alice";
  record.nickname = "Alice";
  record.password_hash = HashPasswordForTest("654321");
  repo.find_by_username_result.user = record;
  chat::UserService service(repo);

  const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
  assert(result.code == chat::ErrorCode::INVALID_CREDENTIALS);
  assert(result.message == "invalid username or password");
}

void TestLoginSuccessReturnsUserDataTokenAndPendingSession()
{
  FakeUserRepository repo;
  FakeSessionManager session_manager;
  repo.find_by_username_result.status = chat::RepositoryStatus::kOk;
  chat::UserRecord record;
  record.id = 10001;
  record.username = "alice";
  record.nickname = "Alice";
  record.password_hash = HashPasswordForTest("123456");
  repo.find_by_username_result.user = record;
  chat::UserService service(repo, session_manager);

  const chat::LoginResult result = service.login(MakeLoginRequest(), 42);
  assert(result.code == chat::ErrorCode::OK);
  assert(result.message == "login success");
  assert(result.data.user_id == 10001);
  assert(result.data.nickname == "Alice");
  assert(result.data.token == "token_10001");
  assert(repo.last_username == "alice");
  assert(!session_manager.bind_called);
  assert(result.session.authenticated);
  assert(result.session.user_id == 10001);
  assert(result.session.username == "alice");
  assert(result.session.token == "token_10001");
}

void TestBindSessionAppliesPendingSession()
{
  FakeSessionManager session_manager;
  chat::UserService service(session_manager);
  chat::ConnectionSession session;
  session.authenticated = true;
  session.user_id = 10001;
  session.username = "alice";
  session.token = "token_10001";

  service.bindSession(42, session);

  assert(session_manager.bind_called);
  assert(session_manager.last_connection_id == 42);
  assert(session_manager.last_session.authenticated);
  assert(session_manager.last_session.user_id == 10001);
  assert(session_manager.last_session.username == "alice");
  assert(session_manager.last_session.token == "token_10001");
}

void TestWhoAmIReturnsUserNotLoggedInWhenSessionMissing()
{
  FakeSessionManager session_manager;
  chat::UserService service(session_manager);

  const chat::WhoAmIResult result = service.whoami(42);
  assert(result.code == chat::ErrorCode::USER_NOT_FOUND);
  assert(result.message == "user not logged in");
}

void TestWhoAmIReturnsSessionDataWhenLoggedIn()
{
  FakeSessionManager session_manager;
  chat::ConnectionSession session;
  session.authenticated = true;
  session.user_id = 10001;
  session.username = "alice";
  session.token = "token_10001";
  session_manager.last_connection_id = 42;
  session_manager.session_for_connection = session;
  chat::UserService service(session_manager);

  const chat::WhoAmIResult result = service.whoami(42);
  assert(result.code == chat::ErrorCode::OK);
  assert(result.message == "ok");
  assert(result.data.user_id == 10001);
  assert(result.data.username == "alice");
  assert(result.data.token == "token_10001");
}

void TestLogoutReturnsUserNotLoggedInWhenSessionMissing()
{
  FakeSessionManager session_manager;
  chat::UserService service(session_manager);

  const chat::LogoutResult result = service.logout(42);
  assert(result.code == chat::ErrorCode::USER_NOT_FOUND);
  assert(result.message == "user not logged in");
}

void TestLogoutClearsSessionWhenLoggedIn()
{
  FakeSessionManager session_manager;
  chat::ConnectionSession session;
  session.authenticated = true;
  session.user_id = 10001;
  session.username = "alice";
  session.token = "token_10001";
  session_manager.last_connection_id = 42;
  session_manager.session_for_connection = session;
  chat::UserService service(session_manager);

  const chat::LogoutResult result = service.logout(42);
  assert(result.code == chat::ErrorCode::OK);
  assert(result.message == "logout success");
  assert(!session_manager.clear_called);
  assert(session_manager.session_for_connection.has_value());
}

void TestClearSessionSilentlyRemovesLoggedInSession()
{
  FakeSessionManager session_manager;
  chat::ConnectionSession session;
  session.authenticated = true;
  session.user_id = 10001;
  session.username = "alice";
  session.token = "token_10001";
  session_manager.last_connection_id = 42;
  session_manager.session_for_connection = session;
  chat::UserService service(session_manager);

  service.clearSession(42);
  assert(session_manager.clear_called);
  assert(!session_manager.session_for_connection.has_value());
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
  TestLoginReturnsInvalidParamForEmptyUsername();
  TestLoginReturnsInvalidParamForEmptyPassword();
  TestLoginReturnsDbQueryFailedWhenLookupFails();
  TestLoginReturnsUserNotFoundWhenUserDoesNotExist();
  TestLoginReturnsInternalErrorWhenRepositoryResultIsIncomplete();
  TestLoginReturnsWrongPasswordWhenHashDoesNotMatch();
  TestLoginSuccessReturnsUserDataTokenAndPendingSession();
  TestBindSessionAppliesPendingSession();
  TestWhoAmIReturnsUserNotLoggedInWhenSessionMissing();
  TestWhoAmIReturnsSessionDataWhenLoggedIn();
  TestLogoutReturnsUserNotLoggedInWhenSessionMissing();
  TestLogoutClearsSessionWhenLoggedIn();
  TestClearSessionSilentlyRemovesLoggedInSession();
  std::cout << "[PASS] user service tests passed\n";
  return 0;
}
