#include "db/message_repository.h"

#include <mysql/mysql.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "db/db_pool.h"

namespace {

constexpr unsigned int kMysqlDuplicateEntryError = 1062;
constexpr unsigned int kMysqlServerGoneError = 2006;
constexpr unsigned int kMysqlServerLost = 2013;

bool IsConnectionError(unsigned int err_no) { return err_no == kMysqlServerGoneError || err_no == kMysqlServerLost; }

struct MysqlResultDeleter {
    void operator()(MYSQL_RES* result) const {
        if (result != nullptr) {
            mysql_free_result(result);
        }
    }
};

using MysqlResultPtr = std::unique_ptr<MYSQL_RES, MysqlResultDeleter>;

struct QueryExecResult {
    bool ok = false;
    chat::RepositoryStatus status = chat::RepositoryStatus::kQueryFailed;
    unsigned int mysql_error = 0;
};

void LogMysqlError(MYSQL* conn, const std::string& action) {
    const unsigned int err_no = conn != nullptr ? mysql_errno(conn) : 0;
    const char* err_msg = conn != nullptr ? mysql_error(conn) : "unknown mysql error";
    std::cerr << "[message_repository] " << action << " failed";
    if (err_no != 0) {
        std::cerr << " errno=" << err_no;
    }
    std::cerr << " message=" << err_msg << std::endl;
}

std::string EscapeSqlValue(MYSQL* conn, const std::string& input) {
    std::vector<char> buffer(input.size() * 2 + 1, '\0');
    const unsigned long escaped_len =
        mysql_real_escape_string(conn, buffer.data(), input.c_str(), static_cast<unsigned long>(input.size()));
    return std::string(buffer.data(), escaped_len);
}

chat::RepositoryStatus MapBorrowError(chat::DbPoolError error) {
    return error == chat::DbPoolError::kBorrowTimeout ? chat::RepositoryStatus::kBorrowTimeout
                                                      : chat::RepositoryStatus::kConnectionUnavailable;
}

QueryExecResult ExecuteQueryWithStatus(chat::PooledConnection& conn, const std::string& query,
                                       const std::string& action) {
    MYSQL* raw_conn = conn->nativeHandle();
    if (mysql_query(raw_conn, query.c_str()) == 0) {
        return {.ok = true, .status = chat::RepositoryStatus::kOk};
    }
    const unsigned int err_no = mysql_errno(raw_conn);
    LogMysqlError(raw_conn, action);
    if (IsConnectionError(err_no)) {
        conn.markBad();
        return {.ok = false, .status = chat::RepositoryStatus::kConnectionUnavailable, .mysql_error = err_no};
    }
    return {.ok = false, .status = chat::RepositoryStatus::kQueryFailed, .mysql_error = err_no};
}

bool ExecuteQuery(chat::PooledConnection& conn, const std::string& query, const std::string& action) {
    return ExecuteQueryWithStatus(conn, query, action).ok;
}

void RollbackTransaction(chat::PooledConnection& conn) {
    if (!ExecuteQuery(conn, "ROLLBACK", "rollback transaction")) {
        conn.markBad();
    }
}

std::optional<chat::MessageRecord> BuildMessageRecord(MYSQL_ROW row, const unsigned long* lengths) {
    if (row == nullptr || lengths == nullptr) {
        return std::nullopt;
    }

    chat::MessageStatus status = chat::MessageStatus::kStored;
    if (!chat::ParseMessageStatus(std::atoi(row[6]), &status)) {
        return std::nullopt;
    }

    chat::MessageRecord record;
    record.id.assign(row[0], lengths[0]);
    record.conversation_id.assign(row[1], lengths[1]);
    record.client_msg_id.assign(row[2], lengths[2]);
    record.from_user_id = std::strtoll(row[3], nullptr, 10);
    record.to_user_id = std::strtoll(row[4], nullptr, 10);
    record.content.assign(row[5], lengths[5]);
    record.status = status;
    record.created_at = std::strtoll(row[7], nullptr, 10);
    record.delivered_at = row[8] != nullptr ? std::strtoll(row[8], nullptr, 10) : 0;
    record.read_at = row[9] != nullptr ? std::strtoll(row[9], nullptr, 10) : 0;
    return record;
}

std::string MessageSelectColumns() {
    return "id, conversation_id, client_msg_id, from_user_id, to_user_id, content, status, created_at, "
           "delivered_at, read_at";
}

chat::FindMessageResult ReadMessageByClientId(chat::PooledConnection& conn, chat::UserId from_user_id,
                                              const std::string& client_msg_id) {
    MYSQL* raw_conn = conn->nativeHandle();
    const std::string escaped_client_msg_id = EscapeSqlValue(raw_conn, client_msg_id);
    const std::string query = "SELECT " + MessageSelectColumns() +
                              " FROM messages WHERE from_user_id=" + std::to_string(from_user_id) +
                              " AND client_msg_id='" + escaped_client_msg_id + "' LIMIT 1";

    if (mysql_query(raw_conn, query.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "select duplicate message");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = chat::RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MysqlResultPtr result(mysql_store_result(raw_conn));
    if (result == nullptr) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "store duplicate message result");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = chat::RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row == nullptr) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }
    auto record = BuildMessageRecord(row, mysql_fetch_lengths(result.get()));
    if (!record.has_value()) {
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }
    return {.status = chat::RepositoryStatus::kOk, .message = record};
}

chat::RepositoryStatus ReadMessageStatusById(chat::PooledConnection& conn, const std::string& message_id,
                                             chat::MessageStatus* status) {
    MYSQL* raw_conn = conn->nativeHandle();
    const std::string escaped_message_id = EscapeSqlValue(raw_conn, message_id);
    const std::string query = "SELECT status FROM messages WHERE id='" + escaped_message_id + "' LIMIT 1";

    if (mysql_query(raw_conn, query.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "select message status");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return chat::RepositoryStatus::kConnectionUnavailable;
        }
        return chat::RepositoryStatus::kQueryFailed;
    }

    MysqlResultPtr result(mysql_store_result(raw_conn));
    if (result == nullptr) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "store message status result");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return chat::RepositoryStatus::kConnectionUnavailable;
        }
        return chat::RepositoryStatus::kQueryFailed;
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row == nullptr) {
        return chat::RepositoryStatus::kNotFound;
    }
    if (!chat::ParseMessageStatus(std::atoi(row[0]), status)) {
        return chat::RepositoryStatus::kQueryFailed;
    }
    return chat::RepositoryStatus::kOk;
}

std::string SingleChatKey(chat::UserId user_a, chat::UserId user_b) {
    const chat::UserId min_id = std::min(user_a, user_b);
    const chat::UserId max_id = std::max(user_a, user_b);
    return std::to_string(min_id) + "_" + std::to_string(max_id);
}

}  // namespace

namespace chat {

MessageRepository::MessageRepository(DbPool* pool) : pool_(pool) {}

FindOrCreateConversationResult MessageRepository::findOrCreateSingleConversation(UserId user_a, UserId user_b) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    MYSQL* raw_conn = conn->nativeHandle();

    const std::string key = SingleChatKey(user_a, user_b);
    const std::string escaped_key = EscapeSqlValue(raw_conn, key);
    const std::string query = "SELECT id FROM conversations WHERE single_chat_key='" + escaped_key + "' LIMIT 1";

    if (mysql_query(raw_conn, query.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "select conversation by key");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = RepositoryStatus::kQueryFailed};
    }

    MysqlResultPtr result(mysql_store_result(raw_conn));
    if (result == nullptr) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "store conversation select result");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = RepositoryStatus::kQueryFailed};
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row != nullptr) {
        unsigned long* lengths = mysql_fetch_lengths(result.get());
        std::string conv_id(row[0], lengths[0]);
        return {.status = RepositoryStatus::kOk, .conversation_id = conv_id, .created = false};
    }

    // Not found, create it
    const std::string conv_id = "conv_" + key;
    const std::string escaped_conv_id = EscapeSqlValue(raw_conn, conv_id);

    QueryExecResult exec_result =
        ExecuteQueryWithStatus(conn, "START TRANSACTION", "start create conversation transaction");
    if (!exec_result.ok) {
        return {.status = exec_result.status};
    }

    const std::string insert_conversation = "INSERT INTO conversations(id, type, single_chat_key) VALUES('" +
                                            escaped_conv_id + "','single','" + escaped_key + "')";
    if (mysql_query(raw_conn, insert_conversation.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        RollbackTransaction(conn);
        if (err_no == kMysqlDuplicateEntryError) {
            // Concurrently created, read again
            if (mysql_query(raw_conn, query.c_str()) == 0) {
                MysqlResultPtr select_res(mysql_store_result(raw_conn));
                if (select_res != nullptr) {
                    MYSQL_ROW r = mysql_fetch_row(select_res.get());
                    if (r != nullptr) {
                        unsigned long* len = mysql_fetch_lengths(select_res.get());
                        return {.status = RepositoryStatus::kOk,
                                .conversation_id = std::string(r[0], len[0]),
                                .created = false};
                    }
                }
            }
            return {.status = RepositoryStatus::kDuplicate};
        }
        LogMysqlError(raw_conn, "insert conversation");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = RepositoryStatus::kInsertFailed};
    }

    const std::string insert_member_a = "INSERT INTO conversation_members(conversation_id, user_id, role) VALUES('" +
                                        escaped_conv_id + "'," + std::to_string(user_a) + ",'member')";
    if (mysql_query(raw_conn, insert_member_a.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        RollbackTransaction(conn);
        LogMysqlError(raw_conn, "insert member a");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = RepositoryStatus::kInsertFailed};
    }

    if (user_a != user_b) {
        const std::string insert_member_b =
            "INSERT INTO conversation_members(conversation_id, user_id, role) VALUES('" + escaped_conv_id + "'," +
            std::to_string(user_b) + ",'member')";
        if (mysql_query(raw_conn, insert_member_b.c_str()) != 0) {
            const unsigned int err_no = mysql_errno(raw_conn);
            RollbackTransaction(conn);
            LogMysqlError(raw_conn, "insert member b");
            if (IsConnectionError(err_no)) {
                conn.markBad();
                return {.status = RepositoryStatus::kConnectionUnavailable};
            }
            return {.status = RepositoryStatus::kInsertFailed};
        }
    }

    exec_result = ExecuteQueryWithStatus(conn, "COMMIT", "commit create conversation transaction");
    if (!exec_result.ok) {
        RollbackTransaction(conn);
        return {.status = exec_result.status};
    }

    return {.status = RepositoryStatus::kOk, .conversation_id = conv_id, .created = true};
}

CreateMessageResult MessageRepository::createMessage(const MessageRecord& message) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    MYSQL* raw_conn = conn->nativeHandle();

    FindMessageResult existing_message = ReadMessageByClientId(conn, message.from_user_id, message.client_msg_id);
    if (existing_message.status == RepositoryStatus::kOk && existing_message.message.has_value()) {
        return {.status = RepositoryStatus::kOk,
                .message_id = existing_message.message->id,
                .message = existing_message.message,
                .created = false};
    }
    if (existing_message.status != RepositoryStatus::kNotFound) {
        return {.status = existing_message.status};
    }

    const std::string escaped_conversation_id = EscapeSqlValue(raw_conn, message.conversation_id);
    const std::string escaped_client_msg_id = EscapeSqlValue(raw_conn, message.client_msg_id);
    const std::string escaped_message_id = EscapeSqlValue(raw_conn, message.id);
    const std::string escaped_content = EscapeSqlValue(raw_conn, message.content);
    const std::string escaped_single_chat_key =
        EscapeSqlValue(raw_conn, SingleChatKey(message.from_user_id, message.to_user_id));

    QueryExecResult exec_result = ExecuteQueryWithStatus(conn, "START TRANSACTION", "start create message transaction");
    if (!exec_result.ok) {
        return {.status = exec_result.status};
    }

    const std::string upsert_conversation = "INSERT INTO conversations(id, type, single_chat_key) VALUES('" +
                                            escaped_conversation_id + "','single','" + escaped_single_chat_key +
                                            "') ON DUPLICATE KEY UPDATE updated_at=updated_at";
    exec_result = ExecuteQueryWithStatus(conn, upsert_conversation, "upsert conversation");
    if (!exec_result.ok) {
        RollbackTransaction(conn);
        return {.status = exec_result.status == RepositoryStatus::kConnectionUnavailable
                              ? exec_result.status
                              : RepositoryStatus::kInsertFailed};
    }

    const std::string upsert_from_member = "INSERT INTO conversation_members(conversation_id, user_id, role) VALUES('" +
                                           escaped_conversation_id + "'," + std::to_string(message.from_user_id) +
                                           ",'member') ON DUPLICATE KEY UPDATE role=role";
    exec_result = ExecuteQueryWithStatus(conn, upsert_from_member, "upsert sender conversation member");
    if (!exec_result.ok) {
        RollbackTransaction(conn);
        return {.status = exec_result.status == RepositoryStatus::kConnectionUnavailable
                              ? exec_result.status
                              : RepositoryStatus::kInsertFailed};
    }

    const std::string upsert_to_member = "INSERT INTO conversation_members(conversation_id, user_id, role) VALUES('" +
                                         escaped_conversation_id + "'," + std::to_string(message.to_user_id) +
                                         ",'member') ON DUPLICATE KEY UPDATE role=role";
    exec_result = ExecuteQueryWithStatus(conn, upsert_to_member, "upsert receiver conversation member");
    if (!exec_result.ok) {
        RollbackTransaction(conn);
        return {.status = exec_result.status == RepositoryStatus::kConnectionUnavailable
                              ? exec_result.status
                              : RepositoryStatus::kInsertFailed};
    }

    const std::string insert_message =
        "INSERT INTO messages(id, conversation_id, client_msg_id, from_user_id, to_user_id, content, status, "
        "created_at, delivered_at, read_at) VALUES('" +
        escaped_message_id + "','" + escaped_conversation_id + "','" + escaped_client_msg_id + "'," +
        std::to_string(message.from_user_id) + "," + std::to_string(message.to_user_id) + ",'" + escaped_content +
        "'," + std::to_string(ToStorageMessageStatus(message.status)) + "," + std::to_string(message.created_at) +
        ",NULL,NULL)";

    if (mysql_query(raw_conn, insert_message.c_str()) != 0) {
        const unsigned int err_no = mysql_errno(raw_conn);
        if (err_no == kMysqlDuplicateEntryError) {
            RollbackTransaction(conn);
            FindMessageResult duplicate_message =
                ReadMessageByClientId(conn, message.from_user_id, message.client_msg_id);
            if (duplicate_message.status == RepositoryStatus::kOk && duplicate_message.message.has_value()) {
                return {.status = RepositoryStatus::kOk,
                        .message_id = duplicate_message.message->id,
                        .message = duplicate_message.message,
                        .created = false};
            }
            if (duplicate_message.status == RepositoryStatus::kNotFound) {
                return {.status = RepositoryStatus::kDuplicate};
            }
            return {.status = duplicate_message.status};
        }
        LogMysqlError(raw_conn, "insert message");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            RollbackTransaction(conn);
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        RollbackTransaction(conn);
        return {.status = RepositoryStatus::kInsertFailed};
    }

    exec_result = ExecuteQueryWithStatus(conn, "COMMIT", "commit create message transaction");
    if (!exec_result.ok) {
        RollbackTransaction(conn);
        return {.status = exec_result.status};
    }

    return {.status = RepositoryStatus::kOk, .message_id = message.id, .message = message, .created = true};
}

FindMessageResult MessageRepository::findMessageByClientMsgId(UserId from_user_id, const std::string& client_msg_id) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    return ReadMessageByClientId(conn, from_user_id, client_msg_id);
}

ListOfflineMessagesResult MessageRepository::listOfflineMessages(UserId to_user_id, int32_t limit,
                                                                 const std::string& cursor) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    MYSQL* raw_conn = conn->nativeHandle();

    const int32_t safe_limit = std::min(limit > 0 ? limit : 20, 100);
    const int32_t query_limit = safe_limit + 1;
    const std::string escaped_cursor = EscapeSqlValue(raw_conn, cursor);
    const std::string query =
        "SELECT " + MessageSelectColumns() + " FROM messages WHERE to_user_id=" + std::to_string(to_user_id) +
        " AND status=" + std::to_string(ToStorageMessageStatus(MessageStatus::kStored)) +
        (cursor.empty() ? ""
                        : " AND (created_at, id) > (SELECT created_at, id FROM messages WHERE id='" + escaped_cursor +
                              "' AND to_user_id=" + std::to_string(to_user_id) + " LIMIT 1)") +
        " ORDER BY created_at ASC, id ASC LIMIT " + std::to_string(query_limit);

    QueryExecResult exec_result = ExecuteQueryWithStatus(conn, query, "list offline messages");
    if (!exec_result.ok) {
        return {.status = exec_result.status};
    }

    MysqlResultPtr result(mysql_store_result(raw_conn));
    if (result == nullptr) {
        const unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "store offline messages result");
        if (IsConnectionError(err_no)) {
            conn.markBad();
            return {.status = RepositoryStatus::kConnectionUnavailable};
        }
        return {.status = RepositoryStatus::kQueryFailed};
    }

    ListOfflineMessagesResult list_result;
    list_result.status = RepositoryStatus::kOk;
    MYSQL_ROW row = nullptr;
    while ((row = mysql_fetch_row(result.get())) != nullptr) {
        auto record = BuildMessageRecord(row, mysql_fetch_lengths(result.get()));
        if (!record.has_value()) {
            return {.status = RepositoryStatus::kQueryFailed};
        }
        if (static_cast<int32_t>(list_result.messages.size()) < safe_limit) {
            list_result.messages.push_back(*record);
        } else {
            list_result.has_more = true;
            break;
        }
    }
    return list_result;
}

MarkDeliveredResult MessageRepository::markDelivered(UserId to_user_id, const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) {
        return {.status = RepositoryStatus::kOk, .affected_rows = 0};
    }

    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    MYSQL* raw_conn = conn->nativeHandle();

    std::string in_clause = "";
    for (size_t i = 0; i < message_ids.size(); ++i) {
        if (i > 0)
            in_clause += ",";
        in_clause += "'" + EscapeSqlValue(raw_conn, message_ids[i]) + "'";
    }

    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string query =
        "UPDATE messages SET status=" + std::to_string(ToStorageMessageStatus(MessageStatus::kDelivered)) +
        ", delivered_at=" + std::to_string(now) + " WHERE id IN (" + in_clause +
        ") AND to_user_id=" + std::to_string(to_user_id) +
        " AND status=" + std::to_string(ToStorageMessageStatus(MessageStatus::kStored));

    QueryExecResult exec_result = ExecuteQueryWithStatus(conn, query, "batch mark delivered");
    if (!exec_result.ok) {
        return {.status = exec_result.status};
    }

    int32_t affected = static_cast<int32_t>(mysql_affected_rows(raw_conn));
    return {.status = RepositoryStatus::kOk, .affected_rows = affected};
}

MarkReadResult MessageRepository::markRead(UserId to_user_id, const std::vector<std::string>& message_ids) {
    if (message_ids.empty()) {
        return {.status = RepositoryStatus::kOk, .affected_rows = 0};
    }

    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        return {.status = MapBorrowError(borrow_res.error)};
    }
    auto& conn = *borrow_res.connection;
    MYSQL* raw_conn = conn->nativeHandle();

    std::string in_clause = "";
    for (size_t i = 0; i < message_ids.size(); ++i) {
        if (i > 0)
            in_clause += ",";
        in_clause += "'" + EscapeSqlValue(raw_conn, message_ids[i]) + "'";
    }

    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string query =
        "UPDATE messages SET delivered_at=CASE WHEN delivered_at IS NULL THEN " + std::to_string(now) +
        " ELSE delivered_at END, status=" + std::to_string(ToStorageMessageStatus(MessageStatus::kRead)) +
        ", read_at=" + std::to_string(now) + " WHERE id IN (" + in_clause +
        ") AND to_user_id=" + std::to_string(to_user_id) + " AND status IN (" +
        std::to_string(ToStorageMessageStatus(MessageStatus::kStored)) + "," +
        std::to_string(ToStorageMessageStatus(MessageStatus::kDelivered)) + ")";

    QueryExecResult exec_result = ExecuteQueryWithStatus(conn, query, "batch mark read");
    if (!exec_result.ok) {
        return {.status = exec_result.status};
    }

    int32_t affected = static_cast<int32_t>(mysql_affected_rows(raw_conn));
    return {.status = RepositoryStatus::kOk, .affected_rows = affected};
}

}  // namespace chat
