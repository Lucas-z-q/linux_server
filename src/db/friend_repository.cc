#include "db/friend_repository.h"

#include <mysql/mysql.h>

#include <algorithm>
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
    LOG_ERROR("FriendRepository") << "action=" << action << " failed mysql_errno=" << error;
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

std::pair<chat::UserId, chat::UserId> OrderedPair(chat::UserId user_a, chat::UserId user_b) {
    return {std::min(user_a, user_b), std::max(user_a, user_b)};
}

struct FriendshipResultBuffers {
    std::int64_t id = 0;
    std::int64_t requester_id = 0;
    std::int64_t addressee_id = 0;
    std::array<char, 21> status{};
    std::array<char, 32> created_at{};
    std::array<char, 32> updated_at{};
    std::array<unsigned long, 6> lengths{};
    std::array<MYSQL_BIND, 6> bindings{};

    FriendshipResultBuffers() {
        bindings[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[0].buffer = &id;
        bindings[1].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[1].buffer = &requester_id;
        bindings[2].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[2].buffer = &addressee_id;
        BindStringResult(&bindings[3], &status, &lengths[3]);
        BindStringResult(&bindings[4], &created_at, &lengths[4]);
        BindStringResult(&bindings[5], &updated_at, &lengths[5]);
    }

    std::optional<chat::FriendshipRecord> Build() const {
        chat::FriendshipStatus parsed_status = chat::FriendshipStatus::kPending;
        if (!chat::ParseFriendshipStatus(std::string(status.data(), lengths[3]), &parsed_status)) {
            return std::nullopt;
        }
        chat::FriendshipRecord record;
        record.id = id;
        record.requester_id = requester_id;
        record.addressee_id = addressee_id;
        record.status = parsed_status;
        record.created_at.assign(created_at.data(), lengths[4]);
        record.updated_at.assign(updated_at.data(), lengths[5]);
        return record;
    }
};

std::string FriendshipSelectColumns() { return "id, requester_id, addressee_id, status, created_at, updated_at"; }

chat::FindFriendshipResult ReadFriendship(chat::PooledConnection& connection, const std::string& sql,
                                          std::vector<chat::StatementParam> params) {
    chat::MysqlStatement statement(connection->nativeHandle());
    if (!statement.Prepare(sql) || !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status =
                    StatementFailure(connection, statement, "select friendship", chat::RepositoryStatus::kQueryFailed)};
    }

    FriendshipResultBuffers buffers;
    if (!statement.BindResult(buffers.bindings.data())) {
        return {.status = StatementFailure(connection, statement, "bind friendship result",
                                           chat::RepositoryStatus::kQueryFailed)};
    }
    const int fetch_result = statement.Fetch();
    if (fetch_result == MYSQL_NO_DATA) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    if (fetch_result != 0) {
        return {.status =
                    StatementFailure(connection, statement, "fetch friendship", chat::RepositoryStatus::kQueryFailed)};
    }
    const auto friendship = buffers.Build();
    if (!friendship) {
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }
    return {.status = chat::RepositoryStatus::kOk, .friendship = friendship};
}

chat::RepositoryStatus ExecuteUpdate(chat::PooledConnection& connection, const std::string& sql,
                                     std::vector<chat::StatementParam> params, const std::string& action,
                                     chat::RepositoryStatus fallback, my_ulonglong* affected_rows = nullptr,
                                     unsigned int* mysql_error = nullptr) {
    chat::MysqlStatement statement(connection->nativeHandle());
    if (!statement.Prepare(sql) || !statement.Execute(&params)) {
        if (mysql_error != nullptr) {
            *mysql_error = statement.ErrorNumber();
        }
        return StatementFailure(connection, statement, action, fallback);
    }
    if (affected_rows != nullptr) {
        *affected_rows = statement.AffectedRows();
    }
    return chat::RepositoryStatus::kOk;
}

}  // namespace

namespace chat {

FriendRepository::FriendRepository(DbPool* pool) : pool_(pool) {}

FindFriendshipResult FriendRepository::findFriendship(UserId user_a, UserId user_b) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    const auto [low, high] = OrderedPair(user_a, user_b);
    const std::string sql =
        "SELECT " + FriendshipSelectColumns() + " FROM friendships WHERE user_low_id=? AND user_high_id=? LIMIT 1";
    return ReadFriendship(*borrow_result.connection, sql, {StatementParam::Int64(low), StatementParam::Int64(high)});
}

CreateFriendshipResult FriendRepository::createFriendRequest(UserId requester_id, UserId addressee_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const auto [low, high] = OrderedPair(requester_id, addressee_id);
    unsigned int mysql_error = 0;
    const RepositoryStatus status =
        ExecuteUpdate(connection,
                      "INSERT INTO friendships(requester_id, addressee_id, user_low_id, user_high_id, status) "
                      "VALUES(?,?,?,?, 'pending')",
                      {StatementParam::Int64(requester_id), StatementParam::Int64(addressee_id),
                       StatementParam::Int64(low), StatementParam::Int64(high)},
                      "insert friendship", RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        if (mysql_error == kMysqlDuplicateEntryError) {
            return {.status = RepositoryStatus::kDuplicate};
        }
        return {.status = status};
    }
    FindFriendshipResult created = ReadFriendship(
        connection, "SELECT " + FriendshipSelectColumns() + " FROM friendships WHERE id=LAST_INSERT_ID() LIMIT 1", {});
    if (created.status != RepositoryStatus::kOk || !created.friendship) {
        return {.status = created.status};
    }
    return {.status = RepositoryStatus::kOk, .friendship = created.friendship, .created = true};
}

UpdateFriendshipResult FriendRepository::acceptFriendRequest(UserId requester_id, UserId addressee_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    my_ulonglong affected_rows = 0;
    const RepositoryStatus status =
        ExecuteUpdate(connection,
                      "UPDATE friendships SET status='accepted' "
                      "WHERE requester_id=? AND addressee_id=? AND status='pending'",
                      {StatementParam::Int64(requester_id), StatementParam::Int64(addressee_id)}, "accept friendship",
                      RepositoryStatus::kQueryFailed, &affected_rows);
    if (status != RepositoryStatus::kOk) {
        return {.status = status};
    }
    if (affected_rows != 1) {
        return {.status = RepositoryStatus::kNotFound};
    }
    const auto [low, high] = OrderedPair(requester_id, addressee_id);
    const FindFriendshipResult updated = ReadFriendship(
        connection,
        "SELECT " + FriendshipSelectColumns() + " FROM friendships WHERE user_low_id=? AND user_high_id=? LIMIT 1",
        {StatementParam::Int64(low), StatementParam::Int64(high)});
    return {.status = updated.status, .friendship = updated.friendship};
}

DeleteFriendshipResult FriendRepository::deleteFriendship(UserId user_a, UserId user_b) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const auto [low, high] = OrderedPair(user_a, user_b);
    my_ulonglong affected_rows = 0;
    const RepositoryStatus status =
        ExecuteUpdate(connection, "DELETE FROM friendships WHERE user_low_id=? AND user_high_id=?",
                      {StatementParam::Int64(low), StatementParam::Int64(high)}, "delete friendship",
                      RepositoryStatus::kQueryFailed, &affected_rows);
    if (status != RepositoryStatus::kOk) {
        return {.status = status};
    }
    return {.status = affected_rows == 0 ? RepositoryStatus::kNotFound : RepositoryStatus::kOk,
            .affected_rows = static_cast<int32_t>(affected_rows)};
}

ListFriendshipsResult FriendRepository::listFriendships(UserId user_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    MysqlStatement statement(connection->nativeHandle());
    std::vector<StatementParam> params = {
        StatementParam::Int64(user_id),
        StatementParam::Int64(user_id),
        StatementParam::Int64(user_id),
    };
    if (!statement.Prepare("SELECT f.id, f.requester_id, f.addressee_id, f.status, f.created_at, f.updated_at, "
                           "u.id, u.username, u.nickname "
                           "FROM friendships f "
                           "JOIN users u ON u.id = IF(f.requester_id=?, f.addressee_id, f.requester_id) "
                           "WHERE f.requester_id=? OR f.addressee_id=? "
                           "ORDER BY f.updated_at DESC, f.id DESC") ||
        !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status = StatementFailure(connection, statement, "list friendships", RepositoryStatus::kQueryFailed)};
    }

    std::int64_t id = 0;
    std::int64_t requester_id = 0;
    std::int64_t addressee_id = 0;
    std::int64_t peer_id = 0;
    std::array<char, 21> status{};
    std::array<char, 32> created_at{};
    std::array<char, 32> updated_at{};
    std::array<char, 65> username{};
    std::array<char, 65> nickname{};
    std::array<unsigned long, 9> lengths{};
    std::array<MYSQL_BIND, 9> bindings{};
    bindings[0].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[0].buffer = &id;
    bindings[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[1].buffer = &requester_id;
    bindings[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[2].buffer = &addressee_id;
    BindStringResult(&bindings[3], &status, &lengths[3]);
    BindStringResult(&bindings[4], &created_at, &lengths[4]);
    BindStringResult(&bindings[5], &updated_at, &lengths[5]);
    bindings[6].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[6].buffer = &peer_id;
    BindStringResult(&bindings[7], &username, &lengths[7]);
    BindStringResult(&bindings[8], &nickname, &lengths[8]);

    if (!statement.BindResult(bindings.data())) {
        return {.status =
                    StatementFailure(connection, statement, "bind friendship list", RepositoryStatus::kQueryFailed)};
    }

    ListFriendshipsResult result;
    result.status = RepositoryStatus::kOk;
    while (true) {
        const int fetch_result = statement.Fetch();
        if (fetch_result == MYSQL_NO_DATA) {
            break;
        }
        if (fetch_result != 0) {
            return {.status = StatementFailure(connection, statement, "fetch friendship list",
                                               RepositoryStatus::kQueryFailed)};
        }
        FriendshipStatus parsed_status = FriendshipStatus::kPending;
        if (!ParseFriendshipStatus(std::string(status.data(), lengths[3]), &parsed_status)) {
            return {.status = RepositoryStatus::kQueryFailed};
        }
        FriendshipListItem item;
        item.user_id = peer_id;
        item.username.assign(username.data(), lengths[7]);
        item.nickname.assign(nickname.data(), lengths[8]);
        item.status = parsed_status;
        item.direction = requester_id == user_id ? "outgoing" : "incoming";
        item.created_at.assign(created_at.data(), lengths[4]);
        item.updated_at.assign(updated_at.data(), lengths[5]);
        result.friends.push_back(item);
    }
    return result;
}

}  // namespace chat
