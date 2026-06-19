#include "db/user_repository.h"

#include <cassert>
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

void TestFindReturnsEmptyWhenConfigIncomplete() {
    chat::DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3307;
    config.username = "";
    config.password = "";
    config.database = "";

    chat::DbPool pool(config);
    chat::UserRepository repo(&pool);
    const chat::FindUserResult by_name = repo.findByUsername("alice");
    const chat::FindUserResult by_id = repo.findById(1);

    assert(by_name.status == chat::RepositoryStatus::kConnectionUnavailable);
    assert(!by_name.user.has_value());
    assert(by_id.status == chat::RepositoryStatus::kConnectionUnavailable);
    assert(!by_id.user.has_value());
}

void TestCreateReturnsFalseWhenConfigIncomplete() {
    chat::DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3307;
    config.username = "";
    config.password = "";
    config.database = "";

    chat::DbPool pool(config);
    chat::UserRepository repo(&pool);
    const chat::CreateUserResult result = repo.createUser("alice", "hash_xxx", "Alice");

    assert(result.status == chat::RepositoryStatus::kConnectionUnavailable);
    assert(result.user_id == 0);
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

    chat::UserRepository repo(&pool);

    // 2. 借出唯一连接，占用它
    auto conn1 = pool.borrow();
    assert(conn1.ok());

    // 3. 此时调用 UserRepository 方法，由于连接池已满，获取连接会超时
    const chat::FindUserResult by_name = repo.findByUsername("alice");
    const chat::FindUserResult by_id = repo.findById(1);
    const chat::CreateUserResult create_res = repo.createUser("alice", "hash_xxx", "Alice");

    // 4. 验证是否被正确映射为 kBorrowTimeout
    assert(by_name.status == chat::RepositoryStatus::kBorrowTimeout);
    assert(by_id.status == chat::RepositoryStatus::kBorrowTimeout);
    assert(create_res.status == chat::RepositoryStatus::kBorrowTimeout);
}

}  // namespace

int main() {
    TestFindReturnsEmptyWhenConfigIncomplete();
    TestCreateReturnsFalseWhenConfigIncomplete();
    TestRepositoryMapsBorrowTimeout();
    std::cout << "[PASS] user repository tests passed\n";
    return 0;
}
