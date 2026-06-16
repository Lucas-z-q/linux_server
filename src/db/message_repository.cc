#include "db/message_repository.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <optional>
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
    LOG_ERROR("MessageRepository") << "action=" << action << " failed mysql_errno=" << error;
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
    LOG_ERROR("MessageRepository") << "action=" << action << " failed mysql_errno=" << error;
    if (IsConnectionError(error)) {
        connection.markBad();
    }
    return false;
}

void Rollback(chat::PooledConnection& connection) {
    if (!ExecuteCommand(connection, "ROLLBACK", "rollback transaction")) {
        connection.markBad();
    }
}

bool Begin(chat::PooledConnection& connection) {
    return ExecuteCommand(connection, "START TRANSACTION", "start transaction");
}

bool Commit(chat::PooledConnection& connection) { return ExecuteCommand(connection, "COMMIT", "commit transaction"); }

template <std::size_t N>
void BindStringResult(MYSQL_BIND* binding, std::array<char, N>* buffer, unsigned long* length) {
    std::memset(binding, 0, sizeof(*binding));
    binding->buffer_type = MYSQL_TYPE_STRING;
    binding->buffer = buffer->data();
    binding->buffer_length = buffer->size();
    binding->length = length;
}

struct MessageResultBuffers {
    std::array<char, 65> id{};
    std::array<char, 65> conversation_id{};
    std::array<char, 65> client_msg_id{};
    std::int64_t from_user_id = 0;
    std::int64_t to_user_id = 0;
    std::array<char, 4097> content{};
    std::int32_t status = 0;
    std::int64_t created_at = 0;
    std::int64_t delivered_at = 0;
    std::int64_t read_at = 0;
    bool delivered_is_null = false;
    bool read_is_null = false;
    std::array<unsigned long, 10> lengths{};
    std::array<MYSQL_BIND, 10> bindings{};

    MessageResultBuffers() {
        BindStringResult(&bindings[0], &id, &lengths[0]);
        BindStringResult(&bindings[1], &conversation_id, &lengths[1]);
        BindStringResult(&bindings[2], &client_msg_id, &lengths[2]);
        bindings[3].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[3].buffer = &from_user_id;
        bindings[4].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[4].buffer = &to_user_id;
        BindStringResult(&bindings[5], &content, &lengths[5]);
        bindings[6].buffer_type = MYSQL_TYPE_LONG;
        bindings[6].buffer = &status;
        bindings[7].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[7].buffer = &created_at;
        bindings[8].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[8].buffer = &delivered_at;
        bindings[8].is_null = &delivered_is_null;
        bindings[9].buffer_type = MYSQL_TYPE_LONGLONG;
        bindings[9].buffer = &read_at;
        bindings[9].is_null = &read_is_null;
    }

    std::optional<chat::MessageRecord> Build() const {
        chat::MessageStatus parsed_status = chat::MessageStatus::kStored;
        if (!chat::ParseMessageStatus(status, &parsed_status)) {
            return std::nullopt;
        }
        chat::MessageRecord record;
        record.id.assign(id.data(), lengths[0]);
        record.conversation_id.assign(conversation_id.data(), lengths[1]);
        record.client_msg_id.assign(client_msg_id.data(), lengths[2]);
        record.from_user_id = from_user_id;
        record.to_user_id = to_user_id;
        record.content.assign(content.data(), lengths[5]);
        record.status = parsed_status;
        record.created_at = created_at;
        record.delivered_at = delivered_is_null ? 0 : delivered_at;
        record.read_at = read_is_null ? 0 : read_at;
        return record;
    }
};

std::string MessageSelectColumns() {
    return "id, conversation_id, client_msg_id, from_user_id, to_user_id, content, status, created_at, "
           "delivered_at, read_at";
}

chat::FindMessageResult ReadMessageByClientId(chat::PooledConnection& connection, chat::UserId from_user_id,
                                              const std::string& client_msg_id) {
    chat::MysqlStatement statement(connection->nativeHandle());
    std::vector<chat::StatementParam> params = {
        chat::StatementParam::Int64(from_user_id),
        chat::StatementParam::String(client_msg_id),
    };
    const std::string sql =
        "SELECT " + MessageSelectColumns() + " FROM messages WHERE from_user_id=? AND client_msg_id=? LIMIT 1";
    if (!statement.Prepare(sql) || !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status = StatementFailure(connection, statement, "select message by client id",
                                           chat::RepositoryStatus::kQueryFailed)};
    }

    MessageResultBuffers buffers;
    if (!statement.BindResult(buffers.bindings.data())) {
        return {.status = StatementFailure(connection, statement, "bind message result",
                                           chat::RepositoryStatus::kQueryFailed)};
    }
    const int fetch_result = statement.Fetch();
    if (fetch_result == MYSQL_NO_DATA) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    if (fetch_result != 0) {
        return {.status =
                    StatementFailure(connection, statement, "fetch message", chat::RepositoryStatus::kQueryFailed)};
    }
    const auto message = buffers.Build();
    if (!message) {
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }
    return {.status = chat::RepositoryStatus::kOk, .message = message};
}

std::string SingleChatKey(chat::UserId user_a, chat::UserId user_b) {
    return std::to_string(std::min(user_a, user_b)) + "_" + std::to_string(std::max(user_a, user_b));
}

chat::FindOrCreateConversationResult SelectConversation(chat::PooledConnection& connection,
                                                        const std::string& single_chat_key) {
    chat::MysqlStatement statement(connection->nativeHandle());
    std::vector<chat::StatementParam> params = {chat::StatementParam::String(single_chat_key)};
    if (!statement.Prepare("SELECT id FROM conversations WHERE single_chat_key=? LIMIT 1") ||
        !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status = StatementFailure(connection, statement, "select conversation",
                                           chat::RepositoryStatus::kQueryFailed)};
    }

    std::array<char, 65> conversation_id{};
    unsigned long length = 0;
    MYSQL_BIND binding{};
    BindStringResult(&binding, &conversation_id, &length);
    if (!statement.BindResult(&binding)) {
        return {.status = StatementFailure(connection, statement, "bind conversation result",
                                           chat::RepositoryStatus::kQueryFailed)};
    }
    const int fetch_result = statement.Fetch();
    if (fetch_result == MYSQL_NO_DATA) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    if (fetch_result != 0) {
        return {.status = StatementFailure(connection, statement, "fetch conversation",
                                           chat::RepositoryStatus::kQueryFailed)};
    }
    return {.status = chat::RepositoryStatus::kOk,
            .conversation_id = std::string(conversation_id.data(), length),
            .created = false};
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

std::string Placeholders(std::size_t count) {
    std::string placeholders;
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            placeholders += ',';
        }
        placeholders += '?';
    }
    return placeholders;
}

}  // namespace

namespace chat {

MessageRepository::MessageRepository(DbPool* pool) : pool_(pool) {}

FindOrCreateConversationResult MessageRepository::findOrCreateSingleConversation(UserId user_a, UserId user_b) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const std::string key = SingleChatKey(user_a, user_b);
    FindOrCreateConversationResult existing = SelectConversation(connection, key);
    if (existing.status != RepositoryStatus::kNotFound) {
        return existing;
    }

    const std::string conversation_id = "conv_" + key;
    if (!Begin(connection)) {
        return {.status = RepositoryStatus::kQueryFailed};
    }

    unsigned int mysql_error = 0;
    RepositoryStatus status =
        ExecuteUpdate(connection, "INSERT INTO conversations(id, type, single_chat_key) VALUES(?,'single',?)",
                      {StatementParam::String(conversation_id), StatementParam::String(key)}, "insert conversation",
                      RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        if (mysql_error == kMysqlDuplicateEntryError) {
            existing = SelectConversation(connection, key);
            return existing.status == RepositoryStatus::kOk
                       ? existing
                       : FindOrCreateConversationResult{.status = RepositoryStatus::kDuplicate};
        }
        return {.status = status};
    }

    status = ExecuteUpdate(connection,
                           "INSERT INTO conversation_members(conversation_id, user_id, role) "
                           "VALUES(?,?,'member')",
                           {StatementParam::String(conversation_id), StatementParam::Int64(user_a)},
                           "insert conversation member", RepositoryStatus::kInsertFailed);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        return {.status = status};
    }
    if (user_a != user_b) {
        status = ExecuteUpdate(connection,
                               "INSERT INTO conversation_members(conversation_id, user_id, role) "
                               "VALUES(?,?,'member')",
                               {StatementParam::String(conversation_id), StatementParam::Int64(user_b)},
                               "insert conversation member", RepositoryStatus::kInsertFailed);
        if (status != RepositoryStatus::kOk) {
            Rollback(connection);
            return {.status = status};
        }
    }

    if (!Commit(connection)) {
        Rollback(connection);
        return {.status = RepositoryStatus::kQueryFailed};
    }
    return {.status = RepositoryStatus::kOk, .conversation_id = conversation_id, .created = true};
}

CreateMessageResult MessageRepository::createMessage(const MessageRecord& message) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;

    FindMessageResult existing = ReadMessageByClientId(connection, message.from_user_id, message.client_msg_id);
    if (existing.status == RepositoryStatus::kOk && existing.message) {
        return {.status = RepositoryStatus::kOk,
                .message_id = existing.message->id,
                .message = existing.message,
                .created = false};
    }
    if (existing.status != RepositoryStatus::kNotFound) {
        return {.status = existing.status};
    }
    if (!Begin(connection)) {
        return {.status = RepositoryStatus::kQueryFailed};
    }

    RepositoryStatus status =
        ExecuteUpdate(connection,
                      "INSERT INTO conversations(id, type, single_chat_key) VALUES(?,'single',?) "
                      "ON DUPLICATE KEY UPDATE updated_at=updated_at",
                      {StatementParam::String(message.conversation_id),
                       StatementParam::String(SingleChatKey(message.from_user_id, message.to_user_id))},
                      "upsert conversation", RepositoryStatus::kInsertFailed);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        return {.status = status};
    }

    for (const UserId member : {message.from_user_id, message.to_user_id}) {
        status = ExecuteUpdate(connection,
                               "INSERT INTO conversation_members(conversation_id, user_id, role) "
                               "VALUES(?,?,'member') ON DUPLICATE KEY UPDATE role=role",
                               {StatementParam::String(message.conversation_id), StatementParam::Int64(member)},
                               "upsert conversation member", RepositoryStatus::kInsertFailed);
        if (status != RepositoryStatus::kOk) {
            Rollback(connection);
            return {.status = status};
        }
    }

    unsigned int mysql_error = 0;
    status = ExecuteUpdate(
        connection,
        "INSERT INTO messages(id, conversation_id, client_msg_id, from_user_id, to_user_id, content, status, "
        "created_at, delivered_at, read_at) VALUES(?,?,?,?,?,?,?,?,NULL,NULL)",
        {StatementParam::String(message.id), StatementParam::String(message.conversation_id),
         StatementParam::String(message.client_msg_id), StatementParam::Int64(message.from_user_id),
         StatementParam::Int64(message.to_user_id), StatementParam::String(message.content),
         StatementParam::Int32(ToStorageMessageStatus(message.status)), StatementParam::Int64(message.created_at)},
        "insert message", RepositoryStatus::kInsertFailed, nullptr, &mysql_error);
    if (status != RepositoryStatus::kOk) {
        Rollback(connection);
        if (mysql_error == kMysqlDuplicateEntryError) {
            FindMessageResult duplicate =
                ReadMessageByClientId(connection, message.from_user_id, message.client_msg_id);
            if (duplicate.status == RepositoryStatus::kOk && duplicate.message) {
                return {.status = RepositoryStatus::kOk,
                        .message_id = duplicate.message->id,
                        .message = duplicate.message,
                        .created = false};
            }
            return {.status = duplicate.status == RepositoryStatus::kNotFound ? RepositoryStatus::kDuplicate
                                                                              : duplicate.status};
        }
        return {.status = status};
    }

    if (!Commit(connection)) {
        Rollback(connection);
        return {.status = RepositoryStatus::kQueryFailed};
    }
    return {.status = RepositoryStatus::kOk, .message_id = message.id, .message = message, .created = true};
}

FindMessageResult MessageRepository::findMessageByClientMsgId(UserId from_user_id, const std::string& client_msg_id) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    return ReadMessageByClientId(*borrow_result.connection, from_user_id, client_msg_id);
}

ListOfflineMessagesResult MessageRepository::listOfflineMessages(UserId to_user_id, int32_t limit,
                                                                 const std::string& cursor) {
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const int32_t safe_limit = std::min(limit > 0 ? limit : 20, 100);
    const int32_t query_limit = safe_limit + 1;

    std::string sql = "SELECT " + MessageSelectColumns() + " FROM messages WHERE to_user_id=? AND status=?";
    std::vector<StatementParam> params = {
        StatementParam::Int64(to_user_id),
        StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kStored)),
    };
    if (!cursor.empty()) {
        sql +=
            " AND (created_at, id) > "
            "(SELECT created_at, id FROM messages WHERE id=? AND to_user_id=? LIMIT 1)";
        params.push_back(StatementParam::String(cursor));
        params.push_back(StatementParam::Int64(to_user_id));
    }
    sql += " ORDER BY created_at ASC, id ASC LIMIT ?";
    params.push_back(StatementParam::Int32(query_limit));

    MysqlStatement statement(connection->nativeHandle());
    if (!statement.Prepare(sql) || !statement.Execute(&params) || !statement.StoreResult()) {
        return {.status =
                    StatementFailure(connection, statement, "list offline messages", RepositoryStatus::kQueryFailed)};
    }

    MessageResultBuffers buffers;
    if (!statement.BindResult(buffers.bindings.data())) {
        return {.status =
                    StatementFailure(connection, statement, "bind offline messages", RepositoryStatus::kQueryFailed)};
    }

    ListOfflineMessagesResult result;
    result.status = RepositoryStatus::kOk;
    while (true) {
        const int fetch_result = statement.Fetch();
        if (fetch_result == MYSQL_NO_DATA) {
            break;
        }
        if (fetch_result != 0) {
            return {.status = StatementFailure(connection, statement, "fetch offline messages",
                                               RepositoryStatus::kQueryFailed)};
        }
        const auto message = buffers.Build();
        if (!message) {
            return {.status = RepositoryStatus::kQueryFailed};
        }
        if (static_cast<int32_t>(result.messages.size()) == safe_limit) {
            result.has_more = true;
            break;
        }
        result.messages.push_back(*message);
    }
    return result;
}

MarkDeliveredResult MessageRepository::markDelivered(UserId to_user_id, const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) {
        return {.status = RepositoryStatus::kOk, .affected_rows = 0};
    }
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::vector<StatementParam> params = {
        StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kDelivered)),
        StatementParam::Int64(now),
    };
    for (const std::string& message_id : message_ids) {
        params.push_back(StatementParam::String(message_id));
    }
    params.push_back(StatementParam::Int64(to_user_id));
    params.push_back(StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kStored)));

    const std::string sql = "UPDATE messages SET status=?, delivered_at=? WHERE id IN (" +
                            Placeholders(message_ids.size()) + ") AND to_user_id=? AND status=?";
    my_ulonglong affected_rows = 0;
    const RepositoryStatus status = ExecuteUpdate(connection, sql, std::move(params), "mark messages delivered",
                                                  RepositoryStatus::kQueryFailed, &affected_rows);
    return {.status = status, .affected_rows = static_cast<int32_t>(affected_rows)};
}

MarkReadResult MessageRepository::markRead(UserId to_user_id, const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) {
        return {.status = RepositoryStatus::kOk, .affected_rows = 0};
    }
    auto borrow_result = pool_->borrow();
    if (!borrow_result.ok()) {
        return {.status = MapBorrowError(borrow_result.error)};
    }
    auto& connection = *borrow_result.connection;
    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::vector<StatementParam> params = {
        StatementParam::Int64(now),
        StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kRead)),
        StatementParam::Int64(now),
    };
    for (const std::string& message_id : message_ids) {
        params.push_back(StatementParam::String(message_id));
    }
    params.push_back(StatementParam::Int64(to_user_id));
    params.push_back(StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kStored)));
    params.push_back(StatementParam::Int32(ToStorageMessageStatus(MessageStatus::kDelivered)));

    const std::string sql =
        "UPDATE messages SET delivered_at=CASE WHEN delivered_at IS NULL THEN ? "
        "ELSE delivered_at END, status=?, read_at=? WHERE id IN (" +
        Placeholders(message_ids.size()) + ") AND to_user_id=? AND status IN (?,?)";
    my_ulonglong affected_rows = 0;
    const RepositoryStatus status = ExecuteUpdate(connection, sql, std::move(params), "mark messages read",
                                                  RepositoryStatus::kQueryFailed, &affected_rows);
    return {.status = status, .affected_rows = static_cast<int32_t>(affected_rows)};
}

}  // namespace chat
