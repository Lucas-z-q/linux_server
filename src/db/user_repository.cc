#include "db/user_repository.h"

#include <mysql/mysql.h>

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "db/db_pool.h"
#include "db/mysql_statement.h"

namespace {

constexpr unsigned int kMysqlDuplicateEntryError = 1062;
constexpr unsigned int kMysqlServerGoneError = 2006;
constexpr unsigned int kMysqlServerLostError = 2013;

bool IsConnectionError(unsigned int error) { return error == kMysqlServerGoneError || error == kMysqlServerLostError; }

chat::RepositoryStatus MapBorrowError(chat::DbPoolError error) {
    return error == chat::DbPoolError::kBorrowTimeout ? chat::RepositoryStatus::kBorrowTimeout
                                                      : chat::RepositoryStatus::kConnectionUnavailable;
}

chat::RepositoryStatus StatementFailure(chat::PooledConnection& connection, const chat::MysqlStatement& statement,
                                        const std::string& action, chat::RepositoryStatus fallback) {
    const unsigned int error = statement.ErrorNumber();
    LOG_ERROR("UserRepository") << "action=" << action << " failed mysql_errno=" << error;
    if (IsConnectionError(error)) {
        connection.markBad();
        return chat::RepositoryStatus::kConnectionUnavailable;
    }
    return fallback;
}

template <std::size_t N>
void BindStringResult(MYSQL_BIND* binding, std::array<char, N>* buffer, unsigned long* length) {
    std::memset(binding, 0, sizeof(*binding));
    binding->buffer_type = MYSQL_TYPE_STRING;
    binding->buffer = buffer->data();
    binding->buffer_length = buffer->size();
    binding->length = length;
}

chat::FindUserResult ReadUser(chat::PooledConnection& connection, const std::string& sql,
                              std::vector<chat::StatementParam> params) {
    chat::MysqlStatement statement(connection->nativeHandle());
    if (!statement.Prepare(sql) || !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status = StatementFailure(connection, statement, "select user", chat::RepositoryStatus::kQueryFailed)};
    }

    std::int64_t user_id = 0;
    std::int32_t status = 0;
    std::array<char, 65> username{};
    std::array<char, 129> password_hash{};
    std::array<char, 257> nickname{};
    std::array<char, 32> created_at{};
    std::array<char, 32> updated_at{};
    std::array<unsigned long, 7> lengths{};
    std::array<MYSQL_BIND, 7> bindings{};

    bindings[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[0].buffer = &user_id;
    BindStringResult(&bindings[1], &username, &lengths[1]);
    BindStringResult(&bindings[2], &password_hash, &lengths[2]);
    BindStringResult(&bindings[3], &nickname, &lengths[3]);
    bindings[4].buffer_type = MYSQL_TYPE_LONG;
    bindings[4].buffer = &status;
    BindStringResult(&bindings[5], &created_at, &lengths[5]);
    BindStringResult(&bindings[6], &updated_at, &lengths[6]);

    if (!statement.BindResult(bindings.data())) {
        return {.status =
                    StatementFailure(connection, statement, "bind user result", chat::RepositoryStatus::kQueryFailed)};
    }
    const int fetch_result = statement.Fetch();
    if (fetch_result == MYSQL_NO_DATA) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    if (fetch_result != 0) {
        return {.status = StatementFailure(connection, statement, "fetch user", chat::RepositoryStatus::kQueryFailed)};
    }

    chat::UserRecord record;
    record.id = user_id;
    record.username.assign(username.data(), lengths[1]);
    record.password_hash.assign(password_hash.data(), lengths[2]);
    record.nickname.assign(nickname.data(), lengths[3]);
    record.status = status;
    record.created_at.assign(created_at.data(), lengths[5]);
    record.updated_at.assign(updated_at.data(), lengths[6]);
    return {.status = chat::RepositoryStatus::kOk, .user = std::move(record)};
}

}  // namespace

namespace chat {

UserRepository::UserRepository(DbPool* pool) : pool_(pool) {}

FindUserResult UserRepository::findByUsername(const std::string& username) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    return ReadUser(*borrow_result.connection,
                    "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
                    "FROM users WHERE username=? LIMIT 1",
                    {StatementParam::String(username)});
}

FindUserResult UserRepository::findById(UserId user_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    return ReadUser(*borrow_result.connection,
                    "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
                    "FROM users WHERE id=? LIMIT 1",
                    {StatementParam::Int64(user_id)});
}

CreateUserResult UserRepository::createUser(const std::string& username, const std::string& password_hash,
                                            const std::string& nickname) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    MysqlStatement statement(connection->nativeHandle());
    std::vector<StatementParam> params = {
        StatementParam::String(username),
        StatementParam::String(password_hash),
        StatementParam::String(nickname),
    };
    if (!statement.Prepare("INSERT INTO users(username, password_hash, nickname, status) VALUES(?,?,?,1)") ||
        !statement.Execute(&params)) {
        if (statement.ErrorNumber() == kMysqlDuplicateEntryError) {
            return {.status = RepositoryStatus::kDuplicate};
        }
        return {.status = StatementFailure(connection, statement, "insert user", RepositoryStatus::kInsertFailed)};
    }

    const UserId user_id = static_cast<UserId>(statement.InsertId());
    if (user_id <= 0) {
        return {.status = RepositoryStatus::kInsertFailed};
    }
    return {.status = RepositoryStatus::kOk, .user_id = user_id};
}

RepositoryStatus UserRepository::updatePasswordHash(UserId user_id, const std::string& password_hash) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return MapBorrowError(borrow_result.error);
    }
    auto& connection = *borrow_result.connection;
    MysqlStatement statement(connection->nativeHandle());
    std::vector<StatementParam> params = {
        StatementParam::String(password_hash),
        StatementParam::Int64(user_id),
    };
    if (!statement.Prepare("UPDATE users SET password_hash=? WHERE id=?") || !statement.Execute(&params)) {
        return StatementFailure(connection, statement, "update password hash", RepositoryStatus::kQueryFailed);
    }
    return statement.AffectedRows() == 1 ? RepositoryStatus::kOk : RepositoryStatus::kNotFound;
}

}  // namespace chat
