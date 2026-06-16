#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "db/db_pool.h"
#include "db/db_pool_config.h"
#include "db/mysql_statement.h"
#include "db/user_repository.h"
#include "security/password_hasher.h"

namespace {

bool HasDatabaseEnvironment() {
    const char* database = std::getenv("CHAT_DB_NAME");
    const char* test_database = std::getenv("CHAT_TEST_DB_NAME");
    return std::getenv("CHAT_DB_HOST") != nullptr && std::getenv("CHAT_DB_PORT") != nullptr &&
           std::getenv("CHAT_DB_USER") != nullptr && database != nullptr && test_database != nullptr &&
           std::string(database) != test_database;
}

chat::DbConfig LoadDatabaseConfig() {
    chat::DbConfig config;
    config.host = std::getenv("CHAT_DB_HOST");
    config.port = static_cast<std::uint16_t>(std::stoi(std::getenv("CHAT_DB_PORT")));
    config.username = std::getenv("CHAT_DB_USER");
    if (const char* password = std::getenv("CHAT_DB_PASSWORD")) {
        config.password = password;
    }
    config.database = std::getenv("CHAT_TEST_DB_NAME");
    return config;
}

void Cleanup(chat::DbPool* pool, const std::string& prefix) {
    auto borrowed = pool->borrow();
    assert(borrowed.ok());
    chat::MysqlStatement statement((*borrowed.connection)->nativeHandle());
    assert(statement.Prepare("DELETE FROM users WHERE username LIKE ?"));
    std::vector<chat::StatementParam> params = {chat::StatementParam::String(prefix + "%")};
    assert(statement.Execute(&params));
}

void TestPreparedUserRoundTrips() {
    chat::DbPool pool(LoadDatabaseConfig());
    assert(pool.init().success);
    chat::UserRepository repository(&pool);
    const std::string prefix = "security_user_" + std::to_string(getpid()) + "_";
    Cleanup(&pool, prefix);

    chat::BcryptPasswordHasher hasher;
    const auto generated_hash = hasher.Hash("integration-password");
    const auto generated_updated_hash = hasher.Hash("updated-integration-password");
    assert(generated_hash.has_value());
    assert(generated_updated_hash.has_value());
    const std::string secure_hash = *generated_hash;
    const std::string updated_hash = *generated_updated_hash;
    assert(secure_hash.size() == 60);
    const std::string special_username = prefix + "q'\"\\";
    const std::string special_nickname = "quote'\" slash\\ \xe7\x94\xa8\xe6\x88\xb7";

    const auto created = repository.createUser(special_username, secure_hash, special_nickname);
    assert(created.status == chat::RepositoryStatus::kOk);
    assert(created.user_id > 0);

    const auto by_name = repository.findByUsername(special_username);
    assert(by_name.status == chat::RepositoryStatus::kOk);
    assert(by_name.user.has_value());
    assert(by_name.user->username == special_username);
    assert(by_name.user->nickname == special_nickname);
    assert(by_name.user->password_hash == secure_hash);

    const auto by_id = repository.findById(created.user_id);
    assert(by_id.status == chat::RepositoryStatus::kOk);
    assert(by_id.user->username == special_username);

    assert(repository.updatePasswordHash(created.user_id, updated_hash) == chat::RepositoryStatus::kOk);
    const auto updated = repository.findById(created.user_id);
    assert(updated.status == chat::RepositoryStatus::kOk);
    assert(updated.user->password_hash == updated_hash);

    const auto duplicate = repository.createUser(special_username, secure_hash, special_nickname);
    assert(duplicate.status == chat::RepositoryStatus::kDuplicate);

    const std::string tautology_payload = prefix + "' OR 1=1 --";
    const std::string drop_payload = prefix + "'; DROP TABLE users; --";
    assert(repository.createUser(tautology_payload, secure_hash, "payload-one").status == chat::RepositoryStatus::kOk);
    assert(repository.createUser(drop_payload, secure_hash, "payload-two").status == chat::RepositoryStatus::kOk);

    const auto tautology = repository.findByUsername(tautology_payload);
    const auto drop = repository.findByUsername(drop_payload);
    assert(tautology.status == chat::RepositoryStatus::kOk);
    assert(drop.status == chat::RepositoryStatus::kOk);
    assert(tautology.user->username == tautology_payload);
    assert(drop.user->username == drop_payload);
    assert(repository.findByUsername(prefix + "' OR 1=1 -- missing").status == chat::RepositoryStatus::kNotFound);
    assert(repository.findById(created.user_id + 1000000000).status == chat::RepositoryStatus::kNotFound);
    assert(repository.updatePasswordHash(created.user_id + 1000000000, updated_hash) ==
           chat::RepositoryStatus::kNotFound);
    assert(repository.findById(created.user_id).status == chat::RepositoryStatus::kOk);

    Cleanup(&pool, prefix);
    std::cout << "[PASS] user repository prepared statement integration tests passed\n";
}

void TestStatementErrorsKeepRepositorySemantics() {
    chat::DbPoolConfig pool_config;
    pool_config.min_connections = 1;
    pool_config.max_connections = 1;
    chat::DbPool pool(LoadDatabaseConfig(), pool_config);
    assert(pool.init().success);

    {
        auto borrowed = pool.borrow();
        assert(borrowed.ok());
        chat::MysqlStatement statement((*borrowed.connection)->nativeHandle());
        assert(statement.Prepare("CREATE TEMPORARY TABLE users(id BIGINT PRIMARY KEY)"));
        std::vector<chat::StatementParam> params;
        assert(statement.Execute(&params));
    }

    chat::UserRepository repository(&pool);
    assert(repository.findByUsername("statement_error").status == chat::RepositoryStatus::kQueryFailed);
    assert(repository.findById(1).status == chat::RepositoryStatus::kQueryFailed);
    assert(repository.createUser("statement_error", "not-sensitive", "Statement Error").status ==
           chat::RepositoryStatus::kInsertFailed);
    assert(repository.updatePasswordHash(1, "not-sensitive") == chat::RepositoryStatus::kQueryFailed);
}

}  // namespace

int main() {
    if (!HasDatabaseEnvironment()) {
        std::cout << "[SKIP] independent CHAT_TEST_DB_NAME is required\n";
        return 77;
    }
    TestPreparedUserRoundTrips();
    TestStatementErrorsKeepRepositorySemantics();
    return 0;
}
