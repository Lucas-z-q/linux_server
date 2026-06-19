#include <unistd.h>

#include <array>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "db/db_pool.h"
#include "db/message_repository.h"
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

void Execute(chat::DbPool* pool, const std::string& sql, std::vector<chat::StatementParam> params) {
    auto borrowed = pool->borrow();
    assert(borrowed.ok());
    chat::MysqlStatement statement((*borrowed.connection)->nativeHandle());
    assert(statement.Prepare(sql));
    assert(statement.Execute(&params));
}

std::int64_t Count(chat::DbPool* pool, const std::string& sql, std::vector<chat::StatementParam> params) {
    auto borrowed = pool->borrow();
    assert(borrowed.ok());
    chat::MysqlStatement statement((*borrowed.connection)->nativeHandle());
    assert(statement.Prepare(sql));
    assert(statement.Execute(&params));
    assert(statement.StoreResult());
    std::int64_t count = 0;
    MYSQL_BIND binding{};
    binding.buffer_type = MYSQL_TYPE_LONGLONG;
    binding.buffer = &count;
    assert(statement.BindResult(&binding));
    assert(statement.Fetch() == 0);
    return count;
}

void Cleanup(chat::DbPool* pool, const std::string& prefix) {
    Execute(pool, "DELETE FROM conversations WHERE id LIKE ?", {chat::StatementParam::String(prefix + "%")});
    Execute(pool, "DELETE FROM users WHERE username LIKE ?", {chat::StatementParam::String(prefix + "%")});
}

chat::MessageRecord MakeMessage(const std::string& id, const std::string& conversation_id,
                                const std::string& client_msg_id, chat::UserId from_user_id, chat::UserId to_user_id,
                                const std::string& content, chat::Timestamp created_at) {
    chat::MessageRecord message;
    message.id = id;
    message.conversation_id = conversation_id;
    message.client_msg_id = client_msg_id;
    message.from_user_id = from_user_id;
    message.to_user_id = to_user_id;
    message.content = content;
    message.status = chat::MessageStatus::kStored;
    message.created_at = created_at;
    return message;
}

void TestPreparedMessageOperations() {
    chat::DbPool pool(LoadDatabaseConfig());
    assert(pool.init().success);
    chat::UserRepository users(&pool);
    chat::MessageRepository messages(&pool);
    chat::BcryptPasswordHasher hasher;
    const auto password_hash = hasher.Hash("integration-password");
    assert(password_hash.has_value());

    const std::string prefix = "security_msg_" + std::to_string(getpid()) + "_";
    Cleanup(&pool, prefix);
    const auto user_a = users.createUser(prefix + "a", *password_hash, "Sender");
    const auto user_b = users.createUser(prefix + "b", *password_hash, "Receiver");
    const auto user_c = users.createUser(prefix + "c", *password_hash, "Other");
    assert(user_a.status == chat::RepositoryStatus::kOk);
    assert(user_b.status == chat::RepositoryStatus::kOk);
    assert(user_c.status == chat::RepositoryStatus::kOk);

    const std::string conversation_id = prefix + "conv'\"\\";
    const std::string special_id = prefix + "m' OR 1=1 --";
    const std::string special_client_id = prefix + "c';DROP";
    const std::string special_content =
        "quote'\" slash\\ newline\n unicode \xe4\xbd\xa0\xe5\xa5\xbd ; DROP TABLE messages; --";
    const chat::MessageRecord special = MakeMessage(special_id, conversation_id, special_client_id, user_a.user_id,
                                                    user_b.user_id, special_content, 1700000001);

    const auto created = messages.createMessage(special);
    assert(created.status == chat::RepositoryStatus::kOk);
    assert(created.created);
    assert(created.message_id == special_id);

    const auto found = messages.findMessageByClientMsgId(user_a.user_id, special_client_id);
    assert(found.status == chat::RepositoryStatus::kOk);
    assert(found.message.has_value());
    assert(found.message->id == special_id);
    assert(found.message->conversation_id == conversation_id);
    assert(found.message->client_msg_id == special_client_id);
    assert(found.message->content == special_content);

    const auto duplicate = messages.createMessage(special);
    assert(duplicate.status == chat::RepositoryStatus::kOk);
    assert(!duplicate.created);
    assert(duplicate.message_id == special_id);

    std::vector<std::string> ids = {special_id};
    for (int index = 2; index <= 6; ++index) {
        const std::string suffix = std::to_string(index);
        const chat::MessageRecord message =
            MakeMessage(prefix + "m" + suffix, conversation_id, prefix + "c" + suffix, user_a.user_id, user_b.user_id,
                        "content-" + suffix, 1700000000 + index);
        const auto result = messages.createMessage(message);
        assert(result.status == chat::RepositoryStatus::kOk);
        assert(result.created);
        ids.push_back(message.id);
    }

    const std::string other_conversation = prefix + "other_conv";
    const chat::MessageRecord other = MakeMessage(prefix + "other_msg", other_conversation, prefix + "other_client",
                                                  user_a.user_id, user_c.user_id, "other", 1700000010);
    assert(messages.createMessage(other).status == chat::RepositoryStatus::kOk);
    const auto wrong_cursor = messages.listOfflineMessages(user_c.user_id, 10, special_id);
    assert(wrong_cursor.status == chat::RepositoryStatus::kOk);
    assert(wrong_cursor.messages.empty());
    const auto malicious_cursor = messages.listOfflineMessages(user_b.user_id, 10, "' OR 1=1 --");
    assert(malicious_cursor.status == chat::RepositoryStatus::kOk);
    assert(malicious_cursor.messages.empty());

    const auto listed = messages.listOfflineMessages(user_b.user_id, 10, "");
    assert(listed.status == chat::RepositoryStatus::kOk);
    assert(listed.messages.size() == 6);
    assert(listed.messages.front().content == special_content);

    assert(messages.markDelivered(user_b.user_id, {}).affected_rows == 0);
    assert(messages.markDelivered(user_b.user_id, {"' OR 1=1 --"}).affected_rows == 0);
    const auto after_malicious_update = messages.findMessageByClientMsgId(user_a.user_id, prefix + "c2");
    assert(after_malicious_update.status == chat::RepositoryStatus::kOk);
    assert(after_malicious_update.message->status == chat::MessageStatus::kStored);
    assert(messages.markDelivered(user_b.user_id, {ids[0]}).affected_rows == 1);
    assert(messages.markDelivered(user_b.user_id, {ids[1], ids[2]}).affected_rows == 2);
    assert(messages.markDelivered(user_b.user_id, {ids[3], ids[4], ids[5]}).affected_rows == 3);

    assert(messages.markRead(user_b.user_id, {}).affected_rows == 0);
    assert(messages.markRead(user_b.user_id, {"'; UPDATE messages SET status=2; --"}).affected_rows == 0);
    const auto after_malicious_read = messages.findMessageByClientMsgId(user_a.user_id, prefix + "c2");
    assert(after_malicious_read.status == chat::RepositoryStatus::kOk);
    assert(after_malicious_read.message->status == chat::MessageStatus::kDelivered);
    assert(messages.markRead(user_b.user_id, {ids[0]}).affected_rows == 1);
    assert(messages.markRead(user_b.user_id, {ids[1], ids[2]}).affected_rows == 2);
    assert(messages.markRead(user_b.user_id, {ids[3], ids[4], ids[5]}).affected_rows == 3);

    const std::string rollback_conversation = prefix + "rollback_conv";
    const chat::MessageRecord invalid =
        MakeMessage(prefix + "rollback_msg", rollback_conversation, prefix + "rollback_client", user_a.user_id,
                    user_c.user_id + 1000000000, "rollback", 1700000020);
    const auto failed = messages.createMessage(invalid);
    assert(failed.status == chat::RepositoryStatus::kInsertFailed);
    assert(Count(&pool, "SELECT COUNT(*) FROM conversations WHERE id=?",
                 {chat::StatementParam::String(rollback_conversation)}) == 0);
    assert(Count(&pool, "SELECT COUNT(*) FROM conversation_members WHERE conversation_id=?",
                 {chat::StatementParam::String(rollback_conversation)}) == 0);
    assert(Count(&pool, "SELECT COUNT(*) FROM messages WHERE id=?", {chat::StatementParam::String(invalid.id)}) == 0);

    Cleanup(&pool, prefix);
    std::cout << "[PASS] message repository security integration tests passed\n";
}

}  // namespace

int main() {
    if (!HasDatabaseEnvironment()) {
        std::cout << "[SKIP] independent CHAT_TEST_DB_NAME is required\n";
        return 77;
    }
    TestPreparedMessageOperations();
    return 0;
}
