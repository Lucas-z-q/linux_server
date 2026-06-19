#include "db/group_repository.h"

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
    LOG_ERROR("GroupRepository") << "action=" << action << " failed mysql_errno=" << error;
    if (IsConnectionError(error)) {
        connection.markBad();
        return chat::RepositoryStatus::kConnectionUnavailable;
    }
    return fallback;
}

bool ExecuteCommand(chat::PooledConnection& connection, const std::string& sql, const std::string& action) {
    MYSQL* raw_connection = connection->nativeHandle();
    if (mysql_query(raw_connection, sql.c_str()) == 0) {
        return true;
    }
    const unsigned int error = mysql_errno(raw_connection);
    LOG_ERROR("GroupRepository") << "action=" << action << " failed mysql_errno=" << error;
    if (IsConnectionError(error)) {
        connection.markBad();
    }
    return false;
}

bool Begin(chat::PooledConnection& connection) {
    return ExecuteCommand(connection, "START TRANSACTION", "start transaction");
}

bool Commit(chat::PooledConnection& connection) { return ExecuteCommand(connection, "COMMIT", "commit transaction"); }

void Rollback(chat::PooledConnection& connection) { ExecuteCommand(connection, "ROLLBACK", "rollback transaction"); }

template <std::size_t N>
void BindStringResult(MYSQL_BIND* binding, std::array<char, N>* buffer, unsigned long* length) {
    std::memset(binding, 0, sizeof(*binding));
    binding->buffer_type = MYSQL_TYPE_STRING;
    binding->buffer = buffer->data();
    binding->buffer_length = buffer->size();
    binding->length = length;
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

chat::GroupRepositoryFindResult ReadGroup(chat::PooledConnection& connection, const std::string& group_id) {
    chat::MysqlStatement statement(connection->nativeHandle());
    std::vector<chat::StatementParam> params = {chat::StatementParam::String(group_id)};
    if (!statement.Prepare("SELECT id, name, owner_id, conversation_id, created_at, updated_at "
                           "FROM `groups` WHERE id=? LIMIT 1") ||
        !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status =
                    StatementFailure(connection, statement, "select group", chat::RepositoryStatus::kQueryFailed)};
    }

    std::array<char, 65> id{};
    std::array<char, 129> name{};
    std::int64_t owner_id = 0;
    std::array<char, 65> conversation_id{};
    std::array<char, 32> created_at{};
    std::array<char, 32> updated_at{};
    std::array<unsigned long, 6> lengths{};
    std::array<MYSQL_BIND, 6> bindings{};
    BindStringResult(&bindings[0], &id, &lengths[0]);
    BindStringResult(&bindings[1], &name, &lengths[1]);
    bindings[2].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[2].buffer = &owner_id;
    BindStringResult(&bindings[3], &conversation_id, &lengths[3]);
    BindStringResult(&bindings[4], &created_at, &lengths[4]);
    BindStringResult(&bindings[5], &updated_at, &lengths[5]);

    if (!statement.BindResult(bindings.data())) {
        return {.status =
                    StatementFailure(connection, statement, "bind group result", chat::RepositoryStatus::kQueryFailed)};
    }
    const int fetch_result = statement.Fetch();
    if (fetch_result == MYSQL_NO_DATA) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    if (fetch_result != 0) {
        return {.status = StatementFailure(connection, statement, "fetch group", chat::RepositoryStatus::kQueryFailed)};
    }

    chat::GroupRecord group;
    group.id.assign(id.data(), lengths[0]);
    group.name.assign(name.data(), lengths[1]);
    group.owner_id = owner_id;
    group.conversation_id.assign(conversation_id.data(), lengths[3]);
    group.created_at.assign(created_at.data(), lengths[4]);
    group.updated_at.assign(updated_at.data(), lengths[5]);
    return {.status = chat::RepositoryStatus::kOk, .group = group};
}

}  // namespace

namespace chat {

GroupRepository::GroupRepository(DbPool* pool) : pool_(pool) {}

GroupRepositoryCreateResult GroupRepository::createGroup(const GroupRecord& group,
                                                         const std::vector<GroupMemberRecord>& initial_members) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    if (!Begin(connection)) {
        return {.status = RepositoryStatus::kQueryFailed};
    }

    unsigned int mysql_error = 0;
    RepositoryStatus status =
        ExecuteUpdate(connection, "INSERT INTO conversations(id, type, single_chat_key) VALUES(?,'group',NULL)",
                      {StatementParam::String(group.conversation_id)}, "insert group conversation",
                      RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        return {.status = mysql_error == kMysqlDuplicateEntryError ? RepositoryStatus::kDuplicate : status};
    }

    status = ExecuteUpdate(connection, "INSERT INTO `groups`(id, name, owner_id, conversation_id) VALUES(?,?,?,?)",
                           {StatementParam::String(group.id), StatementParam::String(group.name),
                            StatementParam::Int64(group.owner_id), StatementParam::String(group.conversation_id)},
                           "insert group", RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        return {.status = mysql_error == kMysqlDuplicateEntryError ? RepositoryStatus::kDuplicate : status};
    }

    for (const GroupMemberRecord& member : initial_members) {
        status = ExecuteUpdate(connection,
                               "INSERT INTO group_members(group_id, user_id, role) VALUES(?,?,?) "
                               "ON DUPLICATE KEY UPDATE role=VALUES(role)",
                               {StatementParam::String(member.group_id), StatementParam::Int64(member.user_id),
                                StatementParam::String(member.role)},
                               "insert group member", RepositoryStatus::kInsertFailed);
        if (status != RepositoryStatus::kOk) {
            Rollback(connection);
            return {.status = status};
        }
    }

    if (!Commit(connection)) {
        Rollback(connection);
        return {.status = RepositoryStatus::kQueryFailed};
    }
    return {.status = RepositoryStatus::kOk, .group = group, .created = true};
}

GroupRepositoryFindResult GroupRepository::findGroup(const std::string& group_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    return ReadGroup(*borrow_result.connection, group_id);
}

GroupRepositoryAddMemberResult GroupRepository::addMember(const GroupMemberRecord& member) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    unsigned int mysql_error = 0;
    const RepositoryStatus status =
        ExecuteUpdate(connection, "INSERT INTO group_members(group_id, user_id, role) VALUES(?,?,?)",
                      {StatementParam::String(member.group_id), StatementParam::Int64(member.user_id),
                       StatementParam::String(member.role)},
                      "insert group member", RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        return {.status = mysql_error == kMysqlDuplicateEntryError ? RepositoryStatus::kDuplicate : status};
    }
    return {.status = RepositoryStatus::kOk, .member = member, .created = true};
}

GroupRepositoryListMembersResult GroupRepository::listMembers(const std::string& group_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    MysqlStatement statement(connection->nativeHandle());
    std::vector<StatementParam> params = {StatementParam::String(group_id)};
    if (!statement.Prepare("SELECT group_id, user_id, role, joined_at FROM group_members "
                           "WHERE group_id=? ORDER BY joined_at ASC, user_id ASC") ||
        !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status =
                    StatementFailure(connection, statement, "list group members", RepositoryStatus::kQueryFailed)};
    }

    std::array<char, 65> id{};
    std::int64_t user_id = 0;
    std::array<char, 21> role{};
    std::array<char, 32> joined_at{};
    std::array<unsigned long, 4> lengths{};
    std::array<MYSQL_BIND, 4> bindings{};
    BindStringResult(&bindings[0], &id, &lengths[0]);
    bindings[1].buffer_type = MYSQL_TYPE_LONGLONG;
    bindings[1].buffer = &user_id;
    BindStringResult(&bindings[2], &role, &lengths[2]);
    BindStringResult(&bindings[3], &joined_at, &lengths[3]);

    if (!statement.BindResult(bindings.data())) {
        return {.status =
                    StatementFailure(connection, statement, "bind group members", RepositoryStatus::kQueryFailed)};
    }

    GroupRepositoryListMembersResult result;
    result.status = RepositoryStatus::kOk;
    while (true) {
        const int fetch_result = statement.Fetch();
        if (fetch_result == MYSQL_NO_DATA) {
            break;
        }
        if (fetch_result != 0) {
            return {.status =
                        StatementFailure(connection, statement, "fetch group members", RepositoryStatus::kQueryFailed)};
        }
        GroupMemberRecord member;
        member.group_id.assign(id.data(), lengths[0]);
        member.user_id = user_id;
        member.role.assign(role.data(), lengths[2]);
        member.joined_at.assign(joined_at.data(), lengths[3]);
        result.members.push_back(member);
    }
    return result;
}

}  // namespace chat
