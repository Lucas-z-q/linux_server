#include "cache/cached_user_repository.h"

#include <cassert>
#include <iostream>
#include <string>

#include "fake_redis_client.h"

namespace {

class CountingUserRepository : public chat::IUserRepository {
   public:
    chat::UserRecord user{7, "alice", "hash", "Alice", 1, "created", "updated"};
    chat::RepositoryStatus id_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus name_status = chat::RepositoryStatus::kOk;
    int find_id_calls = 0;
    int find_name_calls = 0;

    chat::FindUserResult findByUsername(const std::string &username) override {
        ++find_name_calls;
        if (name_status != chat::RepositoryStatus::kOk || username != user.username) {
            return {.status = name_status};
        }
        return {.status = chat::RepositoryStatus::kOk, .user = user};
    }

    chat::FindUserResult findById(chat::UserId user_id) override {
        ++find_id_calls;
        if (id_status != chat::RepositoryStatus::kOk || user_id != user.id) {
            return {.status = id_status};
        }
        return {.status = chat::RepositoryStatus::kOk, .user = user};
    }

    chat::CreateUserResult createUser(const std::string &username, const std::string &password_hash,
                                      const std::string &nickname) override {
        (void)username;
        (void)password_hash;
        (void)nickname;
        return {.status = chat::RepositoryStatus::kOk, .user_id = user.id};
    }
};

void TestPositiveUserCacheDoesNotStorePasswordHash() {
    CountingUserRepository source;
    chat::test::FakeRedisClient redis;
    chat::CachedUserRepository repository(&source, &redis, {});

    assert(repository.findById(7).status == chat::RepositoryStatus::kOk);
    assert(repository.findById(7).status == chat::RepositoryStatus::kOk);
    assert(source.find_id_calls == 2);
    for (const auto &entry : redis.strings) {
        assert(entry.second.find("password_hash") == std::string::npos);
        assert(entry.second.find("hash") == std::string::npos);
    }
}

void TestUsernameAndNotFoundCaches() {
    CountingUserRepository source;
    chat::test::FakeRedisClient redis;
    chat::CachedUserRepository repository(&source, &redis, {});

    assert(repository.findByUsername("alice").status == chat::RepositoryStatus::kOk);
    assert(repository.findByUsername("alice").status == chat::RepositoryStatus::kOk);
    assert(source.find_name_calls == 2);

    source.id_status = chat::RepositoryStatus::kNotFound;
    assert(repository.findById(99).status == chat::RepositoryStatus::kNotFound);
    assert(repository.findById(99).status == chat::RepositoryStatus::kNotFound);
    assert(source.find_id_calls == 1);
}

void TestCorruptCacheAndRedisFailureFallBack() {
    CountingUserRepository source;
    chat::test::FakeRedisClient redis;
    redis.strings["chat:user:id:7"] = "{broken";
    chat::CachedUserRepository repository(&source, &redis, {});

    assert(repository.findById(7).status == chat::RepositoryStatus::kOk);
    assert(source.find_id_calls == 1);

    redis.fail_commands = true;
    assert(repository.findByUsername("alice").status == chat::RepositoryStatus::kOk);
    assert(source.find_name_calls == 1);
}

void TestCreateInvalidatesPotentialStaleEntries() {
    CountingUserRepository source;
    chat::test::FakeRedisClient redis;
    redis.strings["chat:user:id:7"] = "stale";
    redis.strings["chat:user:name:alice"] = "7";
    chat::CachedUserRepository repository(&source, &redis, {});

    assert(repository.createUser("alice", "hash", "Alice").status == chat::RepositoryStatus::kOk);
    assert(redis.strings.count("chat:user:id:7") == 0);
    assert(redis.strings.count("chat:user:name:alice") == 0);
}

}  // namespace

int main() {
    TestPositiveUserCacheDoesNotStorePasswordHash();
    TestUsernameAndNotFoundCaches();
    TestCorruptCacheAndRedisFailureFallBack();
    TestCreateInvalidatesPotentialStaleEntries();
    std::cout << "[PASS] cached user repository tests passed\n";
    return 0;
}
