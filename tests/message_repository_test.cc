#include "db/message_repository.h"

#include <cassert>
#include <cstdlib>
#include <iostream>

#include "db/db_connection.h"
#include "db/db_connection_factory.h"
#include "db/db_pool.h"

namespace {

class FakeDbConnection : public chat::DbConnection {
   public:
    FakeDbConnection(const chat::DbConfig& config) : chat::DbConnection(config) {}

    chat::DbConnectionResult connect() override { return chat::DbConnectionResult{true}; }
    void close() noexcept override {}
    bool ping() noexcept override { return true; }
    bool isConnected() const noexcept override { return true; }
};

class FakeDbConnectionFactory : public chat::IDbConnectionFactory {
   public:
    std::unique_ptr<chat::DbConnection> createConnection(const chat::DbConfig& config) override {
        return std::make_unique<FakeDbConnection>(config);
    }
};

chat::DbConfig GetTestDbConfig() {
    chat::DbConfig config;
    if (const char* host = std::getenv("CHAT_DB_HOST"))
        config.host = host;
    if (const char* port = std::getenv("CHAT_DB_PORT"))
        config.port = std::stoi(port);
    if (const char* user = std::getenv("CHAT_DB_USER"))
        config.username = user;
    if (const char* pwd = std::getenv("CHAT_DB_PASSWORD"))
        config.password = pwd;
    if (const char* db = std::getenv("CHAT_TEST_DB_NAME"))
        config.database = db;
    return config;
}

bool HasRealDbEnvConfig() {
    return std::getenv("CHAT_DB_HOST") != nullptr && std::getenv("CHAT_DB_PORT") != nullptr &&
           std::getenv("CHAT_DB_USER") != nullptr && std::getenv("CHAT_DB_NAME") != nullptr &&
           std::getenv("CHAT_TEST_DB_NAME") != nullptr &&
           std::string(std::getenv("CHAT_DB_NAME")) != std::getenv("CHAT_TEST_DB_NAME");
}

void TestReturnsUnavailableWhenConfigIncomplete() {
    chat::DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3307;
    config.username = "";
    config.password = "";
    config.database = "";

    chat::DbPool pool(config);
    chat::MessageRepository repo(&pool);

    const auto res_conv = repo.findOrCreateSingleConversation(1, 2);
    assert(res_conv.status == chat::RepositoryStatus::kConnectionUnavailable);

    chat::MessageRecord msg;
    msg.id = "m1";
    msg.from_user_id = 1;
    msg.to_user_id = 2;
    msg.client_msg_id = "c1";
    msg.content = "hello";

    const auto res_create = repo.createMessage(msg);
    assert(res_create.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto res_find = repo.findMessageByClientMsgId(1, "c1");
    assert(res_find.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto res_list = repo.listOfflineMessages(1, 10, "");
    assert(res_list.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto res_deliv = repo.markDelivered(2, {"m1"});
    assert(res_deliv.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto res_read = repo.markRead(2, {"m1"});
    assert(res_read.status == chat::RepositoryStatus::kConnectionUnavailable);
}

void TestRepositoryMapsBorrowTimeout() {
    auto factory = std::make_shared<FakeDbConnectionFactory>();

    chat::DbConfig mock_config;
    mock_config.host = "127.0.0.1";
    mock_config.port = 3306;
    mock_config.username = "root";
    mock_config.database = "test";

    chat::DbPoolConfig pool_config;
    pool_config.max_connections = 1;
    pool_config.min_connections = 1;
    pool_config.borrow_timeout_ms = 50;

    chat::DbPool pool(mock_config, pool_config, factory);
    bool init_ok = pool.init().success;
    assert(init_ok);

    chat::MessageRepository repo(&pool);

    // Borrow the only connection, occupying it
    auto conn1 = pool.borrow();
    assert(conn1.ok());

    // Subsequent database queries will time out borrowing a connection
    const auto res_conv = repo.findOrCreateSingleConversation(1, 2);
    assert(res_conv.status == chat::RepositoryStatus::kBorrowTimeout);

    chat::MessageRecord msg;
    msg.id = "m1";
    msg.from_user_id = 1;
    msg.to_user_id = 2;
    msg.client_msg_id = "c1";
    msg.content = "hello";

    const auto res_create = repo.createMessage(msg);
    assert(res_create.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto res_find = repo.findMessageByClientMsgId(1, "c1");
    assert(res_find.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto res_list = repo.listOfflineMessages(1, 10, "");
    assert(res_list.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto res_deliv = repo.markDelivered(2, {"m1"});
    assert(res_deliv.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto res_read = repo.markRead(2, {"m1"});
    assert(res_read.status == chat::RepositoryStatus::kBorrowTimeout);
}

void TestRealDbBehavior() {
    if (!HasRealDbEnvConfig()) {
        std::cout << "[SKIP] independent CHAT_TEST_DB_NAME is required for legacy DB integration coverage\n";
        return;
    }

    chat::DbConfig config = GetTestDbConfig();
    std::cout << "[INFO] Running real DB integration tests...\n";
    chat::DbPool pool(config);
    auto init_res = pool.init();
    if (!init_res.success) {
        std::cerr << "[ERROR] Failed to initialize configured DB Pool: " << init_res.message << "\n";
    }
    assert(init_res.success);

    chat::MessageRepository repo(&pool);

    // Setup: Insert users 1001, 1002, 1003 into users table to pass FK constraints
    {
        auto borrow_res = pool.borrow();
        if (borrow_res.ok()) {
            MYSQL* raw_conn = (*borrow_res.connection)->nativeHandle();
            mysql_query(
                raw_conn,
                "DELETE FROM messages WHERE from_user_id IN (1001, 1002, 1003) OR to_user_id IN (1001, 1002, 1003)");
            mysql_query(raw_conn, "DELETE FROM conversation_members WHERE user_id IN (1001, 1002, 1003)");
            mysql_query(raw_conn, "DELETE FROM users WHERE id IN (1001, 1002, 1003)");
            mysql_query(raw_conn,
                        "INSERT INTO users(id, username, password_hash, nickname) VALUES(1001, 'test_user_1001', "
                        "'hash', 'U1001')");
            mysql_query(raw_conn,
                        "INSERT INTO users(id, username, password_hash, nickname) VALUES(1002, 'test_user_1002', "
                        "'hash', 'U1002')");
            mysql_query(raw_conn,
                        "INSERT INTO users(id, username, password_hash, nickname) VALUES(1003, 'test_user_1003', "
                        "'hash', 'U1003')");
        }
    }

    // 1. Test findOrCreateSingleConversation
    auto conv_res = repo.findOrCreateSingleConversation(1001, 1002);
    assert(conv_res.status == chat::RepositoryStatus::kOk);
    assert(!conv_res.conversation_id.empty());

    // Call again to verify idempotency and created=false
    auto conv_res2 = repo.findOrCreateSingleConversation(1001, 1002);
    assert(conv_res2.status == chat::RepositoryStatus::kOk);
    assert(conv_res2.conversation_id == conv_res.conversation_id);
    assert(!conv_res2.created);

    // Also create conversation for 1001 and 1003
    auto conv_res1003 = repo.findOrCreateSingleConversation(1001, 1003);
    assert(conv_res1003.status == chat::RepositoryStatus::kOk);

    // 2. Test createMessage & Duplicate client_msg_id fallback
    chat::MessageRecord msg;
    msg.id = "test_msg_1";
    msg.conversation_id = conv_res.conversation_id;
    msg.client_msg_id = "client_msg_uniq_123";
    msg.from_user_id = 1001;
    msg.to_user_id = 1002;
    msg.content = "Hello MySQL";
    msg.status = chat::MessageStatus::kStored;
    msg.created_at = 1600000000;

    auto create_res = repo.createMessage(msg);
    assert(create_res.status == chat::RepositoryStatus::kOk);
    assert(create_res.message_id == "test_msg_1");
    assert(create_res.created);

    // Try to create again with duplicate client_msg_id
    auto create_dup = repo.createMessage(msg);
    assert(create_dup.status == chat::RepositoryStatus::kOk);
    assert(create_dup.message_id == "test_msg_1");
    assert(!create_dup.created);  // fallback to existing

    // 3. Test listOfflineMessages with cursor ownership
    chat::ListOfflineMessagesResult list_res = repo.listOfflineMessages(1002, 10, "");
    assert(list_res.status == chat::RepositoryStatus::kOk);
    bool found = false;
    for (auto& r : list_res.messages) {
        if (r.id == "test_msg_1")
            found = true;
    }
    assert(found);

    // Insert a stored message for user 1003
    chat::MessageRecord msg3;
    msg3.id = "test_msg_3";
    msg3.conversation_id = conv_res1003.conversation_id;
    msg3.client_msg_id = "client_msg_uniq_789";
    msg3.from_user_id = 1001;
    msg3.to_user_id = 1003;
    msg3.content = "Hello User 1003";
    msg3.status = chat::MessageStatus::kStored;
    msg3.created_at = 1600000005;
    auto create_res3 = repo.createMessage(msg3);
    assert(create_res3.status == chat::RepositoryStatus::kOk);

    // Fetch offline messages for user 1003 with no cursor
    chat::ListOfflineMessagesResult list_1003_no_cursor = repo.listOfflineMessages(1003, 10, "");
    assert(list_1003_no_cursor.status == chat::RepositoryStatus::kOk);
    assert(list_1003_no_cursor.messages.size() == 1);
    assert(list_1003_no_cursor.messages[0].id == "test_msg_3");

    // Fetch offline messages for user 1003 using cursor "test_msg_1" (which belongs to user 1002)
    // The query should scope the cursor subquery to receiver (1003), yielding an empty/NULL subquery,
    // which in turn must filter out / return empty result instead of leaking/skipping based on user 1002's cursor.
    chat::ListOfflineMessagesResult list_1003_wrong_cursor = repo.listOfflineMessages(1003, 10, "test_msg_1");
    assert(list_1003_wrong_cursor.status == chat::RepositoryStatus::kOk);
    assert(list_1003_wrong_cursor.messages.empty());  // Proves receiver scope restriction is active!

    // 4. Test batch markDelivered and markRead with ownership boundaries
    // Attempting to mark delivered for recipient 9999 (wrong user)
    auto deliv_wrong = repo.markDelivered(9999, {"test_msg_1"});
    assert(deliv_wrong.status == chat::RepositoryStatus::kOk);
    assert(deliv_wrong.affected_rows == 0);  // Not updated because of ownership boundary

    // Correct recipient 1002
    auto deliv_correct = repo.markDelivered(1002, {"test_msg_1"});
    assert(deliv_correct.status == chat::RepositoryStatus::kOk);
    assert(deliv_correct.affected_rows == 1);

    // 5. Test markRead stored->read sets delivered_at
    // Create another stored message
    chat::MessageRecord msg2 = msg;
    msg2.id = "test_msg_2";
    msg2.client_msg_id = "client_msg_uniq_456";
    auto create_res2 = repo.createMessage(msg2);
    assert(create_res2.status == chat::RepositoryStatus::kOk);

    // Directly mark it as read (Stored -> Read)
    auto read_res = repo.markRead(1002, {"test_msg_2"});
    assert(read_res.status == chat::RepositoryStatus::kOk);
    assert(read_res.affected_rows == 1);

    // Verify delivered_at is set (since it transitioned from Stored directly)
    auto check_msg = repo.findMessageByClientMsgId(1001, "client_msg_uniq_456");
    assert(check_msg.status == chat::RepositoryStatus::kOk);
    assert(check_msg.message.has_value());
    assert(check_msg.message->status == chat::MessageStatus::kRead);
    assert(check_msg.message->read_at > 0);
    assert(check_msg.message->delivered_at > 0);  // Must be set to non-zero!

    // Cleanup
    {
        auto borrow_res = pool.borrow();
        if (borrow_res.ok()) {
            MYSQL* raw_conn = (*borrow_res.connection)->nativeHandle();
            mysql_query(
                raw_conn,
                "DELETE FROM messages WHERE from_user_id IN (1001, 1002, 1003) OR to_user_id IN (1001, 1002, 1003)");
            mysql_query(raw_conn, "DELETE FROM conversation_members WHERE user_id IN (1001, 1002, 1003)");
            mysql_query(raw_conn, ("DELETE FROM conversations WHERE id IN ('" + conv_res.conversation_id + "', '" +
                                   conv_res1003.conversation_id + "')")
                                      .c_str());
            mysql_query(raw_conn, "DELETE FROM users WHERE id IN (1001, 1002, 1003)");
        }
    }
    std::cout << "[INFO] Real DB integration tests passed successfully!\n";
}

}  // namespace

int main() {
    TestReturnsUnavailableWhenConfigIncomplete();
    TestRepositoryMapsBorrowTimeout();
    TestRealDbBehavior();
    std::cout << "[PASS] message repository tests passed\n";
    return 0;
}
