#include "service/user_service.h"

#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <string>

namespace {

class FakeUserRepository : public chat::IUserRepository {
   public:
    chat::FindUserResult find_by_username_result;
    chat::FindUserResult find_by_id_result;
    chat::CreateUserResult create_user_result;
    chat::RepositoryStatus update_password_status = chat::RepositoryStatus::kOk;

    int find_by_username_calls = 0;
    int create_user_calls = 0;
    int update_password_calls = 0;

    std::string last_username;
    std::string last_password_hash;
    std::string last_nickname;
    std::string updated_password_hash;

    chat::FindUserResult findByUsername(const std::string &username) override {
        ++find_by_username_calls;
        last_username = username;
        return find_by_username_result;
    }

    chat::FindUserResult findById(chat::UserId user_id) override {
        (void)user_id;
        return find_by_id_result;
    }

    chat::CreateUserResult createUser(const std::string &username, const std::string &password_hash,
                                      const std::string &nickname) override {
        ++create_user_calls;
        last_username = username;
        last_password_hash = password_hash;
        last_nickname = nickname;
        return create_user_result;
    }

    chat::RepositoryStatus updatePasswordHash(chat::UserId user_id, const std::string &password_hash) override {
        (void)user_id;
        ++update_password_calls;
        updated_password_hash = password_hash;
        return update_password_status;
    }
};

class FakeSessionManager : public chat::ISessionManager {
   public:
    bool bind_result = true;
    bool bind_called = false;
    bool clear_called = false;
    chat::ConnectionId last_connection_id = 0;
    chat::ConnectionSession last_session;
    std::optional<chat::ConnectionSession> session_for_connection;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        bind_called = true;
        last_connection_id = connection_id;
        last_session = session;
        if (bind_result) {
            session_for_connection = session;
        }
        return bind_result;
    }

    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        if (session_for_connection.has_value() && session_for_connection->user_id == user_id) {
            return last_connection_id;
        }
        return std::nullopt;
    }

    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        if (session_for_connection.has_value() && connection_id == last_connection_id) {
            return session_for_connection;
        }
        return std::nullopt;
    }

    void ClearSession(chat::ConnectionId connection_id) override {
        if (session_for_connection.has_value() && connection_id == last_connection_id) {
            clear_called = true;
            session_for_connection.reset();
        }
    }
};

class FakeGlobalSessionStore : public chat::IGlobalSessionStore {
   public:
    bool bind_result = true;
    int bind_count = 0;
    int refresh_count = 0;
    int clear_count = 0;
    int revoke_count = 0;

    bool Bind(chat::ConnectionId connection_id, const chat::ConnectionSession &session,
              chat::Timestamp issued_at) override {
        (void)connection_id;
        (void)session;
        (void)issued_at;
        ++bind_count;
        return bind_result;
    }
    bool Refresh(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        (void)connection_id;
        (void)session;
        ++refresh_count;
        return true;
    }
    bool ClearPresence(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        (void)connection_id;
        (void)session;
        ++clear_count;
        return true;
    }
    bool RevokeToken(const std::string &token) override {
        (void)token;
        ++revoke_count;
        return true;
    }
    std::optional<chat::StoredSessionToken> GetToken(const std::string &token) override {
        (void)token;
        return std::nullopt;
    }
    std::optional<chat::StoredUserPresence> GetPresence(chat::UserId user_id) override {
        (void)user_id;
        return std::nullopt;
    }
};

class FakeRateLimiter : public chat::IRateLimiter {
   public:
    chat::RateLimitResult result;
    int calls = 0;
    std::string last_type;
    std::string last_identity;

    chat::RateLimitResult Allow(const std::string &type, const std::string &identity, std::uint32_t limit,
                                std::uint32_t window_seconds) override {
        (void)limit;
        (void)window_seconds;
        ++calls;
        last_type = type;
        last_identity = identity;
        return result;
    }
};

chat::RegisterRequest MakeRegisterRequest() {
    chat::RegisterRequest req;
    req.username = "alice";
    req.password = "123456";
    req.nickname = "Alice";
    return req;
}

chat::LoginRequest MakeLoginRequest() {
    chat::LoginRequest req;
    req.username = "alice";
    req.password = "123456";
    return req;
}

std::string HashPasswordForTest(const std::string &password) {
    return std::to_string(std::hash<std::string>{}(password));
}

void TestRegisterReturnsInvalidParamForEmptyUsername() {
    FakeUserRepository repo;
    chat::UserService service(repo);

    chat::RegisterRequest req = MakeRegisterRequest();
    req.username.clear();

    const chat::RegisterResult result = service.registerUser(req);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
    assert(result.message == "username length must be between 3 and 32");
}

void TestRegisterReturnsUserAlreadyExistsWhenUserFound() {
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

void TestRegisterReturnsDbQueryFailedWhenLookupFails() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kQueryFailed;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(result.message == "query user failed");
}

void TestRegisterReturnsDbInsertFailedWhenCreateFails() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
    repo.create_user_result.status = chat::RepositoryStatus::kInsertFailed;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::DB_INSERT_FAILED);
    assert(result.message == "create user failed");
}

void TestRegisterReturnsUserAlreadyExistsWhenCreateSeesDuplicate() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
    repo.create_user_result.status = chat::RepositoryStatus::kDuplicate;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::USER_ALREADY_EXISTS);
    assert(result.message == "username already exists");
}

void TestRegisterSuccessReturnsUserIdAndHashesPassword() {
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
    assert(repo.last_password_hash.compare(0, 4, "$2b$") == 0);
}

void TestLoginReturnsInvalidParamForEmptyUsername() {
    FakeUserRepository repo;
    chat::UserService service(repo);

    chat::LoginRequest req = MakeLoginRequest();
    req.username.clear();

    const chat::LoginResult result = service.login(req, 0);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
    assert(result.message == "username length must be between 3 and 32");
}

void TestLoginReturnsInvalidParamForEmptyPassword() {
    FakeUserRepository repo;
    chat::UserService service(repo);

    chat::LoginRequest req = MakeLoginRequest();
    req.password.clear();

    const chat::LoginResult result = service.login(req, 0);
    assert(result.code == chat::ErrorCode::INVALID_PARAM);
    assert(result.message == "password length must be between 1 and 72");
}

void TestLoginReturnsDbQueryFailedWhenLookupFails() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kQueryFailed;
    chat::UserService service(repo);

    const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(result.message == "query user failed");
}

void TestLoginReturnsUserNotFoundWhenUserDoesNotExist() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
    chat::UserService service(repo);

    const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
    assert(result.code == chat::ErrorCode::INVALID_CREDENTIALS);
    assert(result.message == "invalid username or password");
}

void TestLoginReturnsInternalErrorWhenRepositoryResultIsIncomplete() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kOk;
    repo.find_by_username_result.user.reset();
    chat::UserService service(repo);

    const chat::LoginResult result = service.login(MakeLoginRequest(), 0);
    assert(result.code == chat::ErrorCode::INTERNAL_ERROR);
    assert(result.message == "unexpected repository result");
}

void TestLoginReturnsWrongPasswordWhenHashDoesNotMatch() {
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

void TestLoginSuccessReturnsUserDataTokenAndPendingSession() {
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
    assert(result.data.token.size() == 64);
    assert(result.data.token.find("token_") != 0);
    assert(repo.last_username == "alice");
    assert(!session_manager.bind_called);
    assert(result.session.authenticated);
    assert(result.session.user_id == 10001);
    assert(result.session.username == "alice");
    assert(result.session.token == result.data.token);
}

void TestBindSessionAppliesPendingSession() {
    FakeSessionManager session_manager;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);
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

void TestRedisFailureDoesNotUndoLocalBind() {
    FakeSessionManager session_manager;
    FakeGlobalSessionStore global_store;
    global_store.bind_result = false;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager, &global_store);
    chat::ConnectionSession session{true, 10001, "alice", "token"};

    service.bindSession(42, session);

    assert(session_manager.GetSession(42).has_value());
    assert(global_store.bind_count == 1);
}

void TestLogoutRevokesTokenButDisconnectKeepsIt() {
    FakeSessionManager session_manager;
    FakeGlobalSessionStore global_store;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager, &global_store);
    chat::ConnectionSession session{true, 10001, "alice", "token"};

    session_manager.BindSession(42, session);
    service.refreshPresence(42);
    assert(global_store.refresh_count == 1);
    service.clearSession(42);
    assert(global_store.clear_count == 1);
    assert(global_store.revoke_count == 0);

    session_manager.BindSession(77, session);
    service.logoutSession(77);
    assert(global_store.clear_count == 2);
    assert(global_store.revoke_count == 1);
}

void TestWhoAmIReturnsUserNotLoggedInWhenSessionMissing() {
    FakeSessionManager session_manager;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);

    const chat::WhoAmIResult result = service.whoami(42);
    assert(result.code == chat::ErrorCode::USER_NOT_FOUND);
    assert(result.message == "user not logged in");
}

void TestWhoAmIReturnsSessionDataWhenLoggedIn() {
    FakeSessionManager session_manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";
    session_manager.last_connection_id = 42;
    session_manager.session_for_connection = session;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);

    const chat::WhoAmIResult result = service.whoami(42);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.message == "ok");
    assert(result.data.user_id == 10001);
    assert(result.data.username == "alice");
    assert(result.data.token == "token_10001");
}

void TestLogoutReturnsUserNotLoggedInWhenSessionMissing() {
    FakeSessionManager session_manager;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);

    const chat::LogoutResult result = service.logout(42);
    assert(result.code == chat::ErrorCode::USER_NOT_FOUND);
    assert(result.message == "user not logged in");
}

void TestLogoutClearsSessionWhenLoggedIn() {
    FakeSessionManager session_manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";
    session_manager.last_connection_id = 42;
    session_manager.session_for_connection = session;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);

    const chat::LogoutResult result = service.logout(42);
    assert(result.code == chat::ErrorCode::OK);
    assert(result.message == "logout success");
    assert(!session_manager.clear_called);
    assert(session_manager.session_for_connection.has_value());
}

void TestClearSessionSilentlyRemovesLoggedInSession() {
    FakeSessionManager session_manager;
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 10001;
    session.username = "alice";
    session.token = "token_10001";
    session_manager.last_connection_id = 42;
    session_manager.session_for_connection = session;
    FakeUserRepository repo;
    chat::UserService service(repo, session_manager);

    service.clearSession(42);
    assert(session_manager.clear_called);
    assert(!session_manager.session_for_connection.has_value());
}

void TestRegisterReturnsDbQueryFailedWhenConnectionUnavailable() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kConnectionUnavailable;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(result.message == "query user failed");
}

void TestRegisterReturnsDbQueryFailedWhenBorrowTimeout() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kBorrowTimeout;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::DB_QUERY_FAILED);
    assert(result.message == "query user failed");
}

void TestRegisterReturnsDbInsertFailedWhenConnectionUnavailableOnCreate() {
    FakeUserRepository repo;
    repo.find_by_username_result.status = chat::RepositoryStatus::kNotFound;
    repo.create_user_result.status = chat::RepositoryStatus::kConnectionUnavailable;
    chat::UserService service(repo);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest());
    assert(result.code == chat::ErrorCode::DB_INSERT_FAILED);
    assert(result.message == "create user failed");
}

void TestRegisterRateLimitReturnsRetryAfter() {
    FakeUserRepository repo;
    FakeSessionManager session_manager;
    FakeRateLimiter limiter;
    limiter.result = {.allowed = false, .retry_after_seconds = 42};
    chat::UserService service(repo, session_manager, nullptr, &limiter);

    const chat::RegisterResult result = service.registerUser(MakeRegisterRequest(), "10.0.0.8");

    assert(result.code == chat::ErrorCode::RATE_LIMITED);
    assert(result.retry_after_seconds == 42);
    assert(limiter.calls == 1);
    assert(limiter.last_type == "register");
    assert(limiter.last_identity == "10.0.0.8");
    assert(repo.last_username.empty());
}

void TestLoginRateLimitRunsBeforeRepositoryLookup() {
    FakeUserRepository repo;
    FakeSessionManager session_manager;
    FakeRateLimiter limiter;
    limiter.result = {.allowed = false, .retry_after_seconds = 8};
    chat::UserService service(repo, session_manager, nullptr, &limiter);

    const chat::LoginResult result = service.login(MakeLoginRequest(), 42, "10.0.0.9");

    assert(result.code == chat::ErrorCode::RATE_LIMITED);
    assert(result.retry_after_seconds == 8);
    assert(limiter.last_type == "login");
    assert(limiter.last_identity == "10.0.0.9");
    assert(repo.last_username.empty());
}

}  // namespace

int main() {
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
    TestRedisFailureDoesNotUndoLocalBind();
    TestLogoutRevokesTokenButDisconnectKeepsIt();
    TestWhoAmIReturnsUserNotLoggedInWhenSessionMissing();
    TestWhoAmIReturnsSessionDataWhenLoggedIn();
    TestLogoutReturnsUserNotLoggedInWhenSessionMissing();
    TestLogoutClearsSessionWhenLoggedIn();
    TestClearSessionSilentlyRemovesLoggedInSession();
    TestRegisterReturnsDbQueryFailedWhenConnectionUnavailable();
    TestRegisterReturnsDbQueryFailedWhenBorrowTimeout();
    TestRegisterReturnsDbInsertFailedWhenConnectionUnavailableOnCreate();
    TestRegisterRateLimitReturnsRetryAfter();
    TestLoginRateLimitRunsBeforeRepositoryLookup();
    std::cout << "[PASS] user service tests passed\n";
    return 0;
}
