#include <cassert>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "service/user_service.h"

namespace {

class FakeRepository : public chat::IUserRepository {
   public:
    chat::FindUserResult find_result{.status = chat::RepositoryStatus::kNotFound};
    chat::CreateUserResult create_result{.status = chat::RepositoryStatus::kOk, .user_id = 42};
    chat::RepositoryStatus update_result = chat::RepositoryStatus::kOk;
    int find_calls = 0;
    int create_calls = 0;
    int update_calls = 0;
    std::string created_hash;
    std::string updated_hash;

    chat::FindUserResult findByUsername(const std::string&) override {
        ++find_calls;
        return find_result;
    }

    chat::FindUserResult findById(chat::UserId) override { return {.status = chat::RepositoryStatus::kNotFound}; }

    chat::CreateUserResult createUser(const std::string&, const std::string& password_hash,
                                      const std::string&) override {
        ++create_calls;
        created_hash = password_hash;
        return create_result;
    }

    chat::RepositoryStatus updatePasswordHash(chat::UserId, const std::string& password_hash) override {
        ++update_calls;
        updated_hash = password_hash;
        return update_result;
    }
};

class FakeSessionManager : public chat::ISessionManager {
   public:
    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession& session) override {
        sessions[connection_id] = session;
        return true;
    }
    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId) override { return std::nullopt; }
    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        const auto found = sessions.find(connection_id);
        return found == sessions.end() ? std::nullopt : std::optional<chat::ConnectionSession>(found->second);
    }
    void ClearSession(chat::ConnectionId connection_id) override { sessions.erase(connection_id); }

    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;
};

class FakeGlobalSessionStore : public chat::IGlobalSessionStore {
   public:
    bool fail_reads = false;
    std::unordered_map<std::string, chat::StoredSessionToken> tokens;

    bool Bind(chat::ConnectionId, const chat::ConnectionSession& session, chat::Timestamp issued_at) override {
        tokens[session.token] = {session.user_id, session.username, issued_at};
        return true;
    }
    bool Refresh(chat::ConnectionId, const chat::ConnectionSession&) override { return true; }
    bool ClearPresence(chat::ConnectionId, const chat::ConnectionSession&) override { return true; }
    bool RevokeToken(const std::string& token) override {
        tokens.erase(token);
        return true;
    }
    std::optional<chat::StoredSessionToken> GetToken(const std::string& token) override {
        if (fail_reads) {
            return std::nullopt;
        }
        const auto found = tokens.find(token);
        return found == tokens.end() ? std::nullopt : std::optional<chat::StoredSessionToken>(found->second);
    }
    std::optional<chat::StoredUserPresence> GetPresence(chat::UserId) override { return std::nullopt; }
};

class FakePasswordHasher : public chat::IPasswordHasher {
   public:
    std::optional<std::string> hash_result = "$2b$12$test-safe-hash";
    bool verify_result = true;
    bool needs_rehash = false;
    int hash_calls = 0;
    int verify_calls = 0;

    std::optional<std::string> Hash(const std::string&) const override {
        ++const_cast<FakePasswordHasher*>(this)->hash_calls;
        return hash_result;
    }

    bool Verify(const std::string&, const std::string&) const override {
        ++const_cast<FakePasswordHasher*>(this)->verify_calls;
        return verify_result;
    }

    bool NeedsRehash(const std::string&) const override { return needs_rehash; }
};

chat::RegisterRequest RegisterRequest() { return {"alice", "123456", "Alice"}; }

chat::LoginRequest LoginRequest() { return {"alice", "123456"}; }

chat::UserRecord UserWithHash(const std::string& hash) {
    chat::UserRecord user;
    user.id = 42;
    user.username = "alice";
    user.password_hash = hash;
    user.nickname = "Alice";
    user.status = 1;
    return user;
}

void TestRegistrationStoresHashAndStopsOnHashFailure() {
    FakeRepository repository;
    FakeSessionManager sessions;
    FakePasswordHasher hasher;
    chat::UserService service(repository, sessions, nullptr, nullptr, {}, &hasher);

    const auto success = service.registerUser(RegisterRequest());
    assert(success.code == chat::ErrorCode::OK);
    assert(repository.create_calls == 1);
    assert(repository.created_hash == *hasher.hash_result);
    assert(repository.created_hash != RegisterRequest().password);

    FakeRepository failing_repository;
    FakePasswordHasher failing_hasher;
    failing_hasher.hash_result = std::nullopt;
    chat::UserService failing_service(failing_repository, sessions, nullptr, nullptr, {}, &failing_hasher);

    const auto failure = failing_service.registerUser(RegisterRequest());
    assert(failure.code == chat::ErrorCode::INTERNAL_ERROR);
    assert(failure.message == "password hashing failed");
    assert(failing_repository.create_calls == 0);
}

void TestLoginUsesVerifyAndUniformCredentialError() {
    FakeRepository repository;
    repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash("stored-hash")};
    FakeSessionManager sessions;
    FakePasswordHasher hasher;
    chat::UserService service(repository, sessions, nullptr, nullptr, {}, &hasher);

    const auto success = service.login(LoginRequest(), 7);
    assert(success.code == chat::ErrorCode::OK);
    assert(hasher.verify_calls == 1);
    assert(hasher.hash_calls == 0);

    FakeRepository missing_repository;
    chat::UserService missing_service(missing_repository, sessions, nullptr, nullptr, {}, &hasher);
    const auto missing = missing_service.login(LoginRequest(), 7);

    hasher.verify_result = false;
    const auto wrong = service.login(LoginRequest(), 7);
    assert(missing.code == chat::ErrorCode::INVALID_CREDENTIALS);
    assert(wrong.code == chat::ErrorCode::INVALID_CREDENTIALS);
    assert(missing.message == wrong.message);
}

void TestLegacyHashMigrationAndFailureBehavior() {
    const std::string legacy_hash = std::to_string(std::hash<std::string>{}("123456"));
    FakeRepository repository;
    repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash(legacy_hash)};
    FakeSessionManager sessions;
    chat::BcryptPasswordHasher hasher;
    chat::UserService service(repository, sessions, nullptr, nullptr, {}, &hasher);

    const auto migrated = service.login(LoginRequest(), 7);
    assert(migrated.code == chat::ErrorCode::OK);
    assert(repository.update_calls == 1);
    assert(repository.updated_hash.compare(0, 4, "$2b$") == 0);
    assert(hasher.Verify("123456", repository.updated_hash));

    FakeRepository wrong_repository;
    wrong_repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash(legacy_hash)};
    chat::UserService wrong_service(wrong_repository, sessions, nullptr, nullptr, {}, &hasher);
    chat::LoginRequest wrong_request = LoginRequest();
    wrong_request.password = "654321";
    const auto wrong = wrong_service.login(wrong_request, 7);
    assert(wrong.code == chat::ErrorCode::INVALID_CREDENTIALS);
    assert(wrong_repository.update_calls == 0);

    FakeRepository failing_repository;
    failing_repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash(legacy_hash)};
    failing_repository.update_result = chat::RepositoryStatus::kQueryFailed;
    chat::UserService failing_service(failing_repository, sessions, nullptr, nullptr, {}, &hasher);
    const auto failure = failing_service.login(LoginRequest(), 7);
    assert(failure.code == chat::ErrorCode::INTERNAL_ERROR);
    assert(failure.message == "credential upgrade failed");
    assert(failure.data.token.empty());
}

void TestNewHashRejectsLegacyStyleAuthentication() {
    chat::BcryptPasswordHasher hasher;
    const auto secure_hash = hasher.Hash("123456");
    assert(secure_hash.has_value());

    FakeRepository repository;
    repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash(*secure_hash)};
    FakeSessionManager sessions;
    chat::UserService service(repository, sessions, nullptr, nullptr, {}, &hasher);
    chat::LoginRequest request = LoginRequest();
    request.password = std::to_string(std::hash<std::string>{}("123456"));

    const auto result = service.login(request, 7);
    assert(result.code == chat::ErrorCode::INVALID_CREDENTIALS);
    assert(repository.update_calls == 0);
}

void TestInvalidInputNeverCallsRepository() {
    const std::vector<chat::RegisterRequest> invalid_register = {
        {"ab", "123456", "Alice"},
        {"bad name", "123456", "Alice"},
        {"' OR 1=1 --", "123456", "Alice"},
        {"alice", "short", "Alice"},
        {"alice", std::string("abc\0def", 7), "Alice"},
        {"alice", "123456", ""},
        {"alice", "123456", "bad\nname"},
    };
    for (const auto& request : invalid_register) {
        FakeRepository repository;
        chat::UserService service(repository);
        assert(service.registerUser(request).code == chat::ErrorCode::INVALID_PARAM);
        assert(repository.find_calls == 0);
        assert(repository.create_calls == 0);
    }

    const std::vector<chat::LoginRequest> invalid_login = {
        {"ab", "123456"},
        {"bad name", "123456"},
        {"alice", ""},
        {"alice", std::string(73, 'x')},
        {"alice", std::string("abc\0def", 7)},
    };
    for (const auto& request : invalid_login) {
        FakeRepository repository;
        chat::UserService service(repository);
        assert(service.login(request, 7).code == chat::ErrorCode::INVALID_PARAM);
        assert(repository.find_calls == 0);
    }
}

void TestTokensAreRandomAndUnique() {
    FakeRepository repository;
    repository.find_result = {.status = chat::RepositoryStatus::kOk, .user = UserWithHash("stored-hash")};
    FakeSessionManager sessions;
    FakePasswordHasher hasher;
    chat::UserService service(repository, sessions, nullptr, nullptr, {}, &hasher);

    const auto first = service.login(LoginRequest(), 7);
    const auto second = service.login(LoginRequest(), 8);
    assert(first.code == chat::ErrorCode::OK);
    assert(second.code == chat::ErrorCode::OK);
    assert(first.data.token.size() == 64);
    assert(second.data.token.size() == 64);
    assert(first.data.token != second.data.token);
}

void TestRequestTokenRequiresCurrentAllowlistEntry() {
    FakeRepository repository;
    FakeSessionManager sessions;
    FakeGlobalSessionStore global;
    chat::UserService service(repository, sessions, &global);
    const std::string token(64, 'a');
    const chat::ConnectionSession session = {
        .authenticated = true,
        .user_id = 42,
        .username = "alice",
        .token = token,
    };
    assert(sessions.BindSession(7, session));
    global.tokens[token] = {42, "alice", 1};

    assert(service.requestTokenMatches(7, token));
    global.tokens.erase(token);
    assert(!service.requestTokenMatches(7, token));
    global.tokens[token] = {42, "alice", 1};
    global.fail_reads = true;
    assert(!service.requestTokenMatches(7, token));
}

}  // namespace

int main() {
    TestRegistrationStoresHashAndStopsOnHashFailure();
    TestLoginUsesVerifyAndUniformCredentialError();
    TestLegacyHashMigrationAndFailureBehavior();
    TestNewHashRejectsLegacyStyleAuthentication();
    TestInvalidInputNeverCallsRepository();
    TestTokensAreRandomAndUnique();
    TestRequestTokenRequiresCurrentAllowlistEntry();
    std::cout << "[PASS] user service security tests passed\n";
    return 0;
}
