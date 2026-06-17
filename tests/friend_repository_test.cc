#include "db/friend_repository.h"

#include <cassert>
#include <iostream>
#include <memory>

#include "db/db_connection.h"
#include "db/db_connection_factory.h"
#include "db/db_pool.h"

namespace {

class FakeDbConnection : public chat::DbConnection {
   public:
    explicit FakeDbConnection(const chat::DbConfig& config) : chat::DbConnection(config) {}

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

void TestReturnsUnavailableWhenConfigIncomplete() {
    chat::DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3307;
    config.username = "";
    config.password = "";
    config.database = "";

    chat::DbPool pool(config);
    chat::FriendRepository repo(&pool);

    const auto find_result = repo.findFriendship(1, 2);
    assert(find_result.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto create_result = repo.createFriendRequest(1, 2);
    assert(create_result.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto accept_result = repo.acceptFriendRequest(1, 2);
    assert(accept_result.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto delete_result = repo.deleteFriendship(1, 2);
    assert(delete_result.status == chat::RepositoryStatus::kConnectionUnavailable);

    const auto list_result = repo.listFriendships(1);
    assert(list_result.status == chat::RepositoryStatus::kConnectionUnavailable);
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
    assert(pool.init().success);

    chat::FriendRepository repo(&pool);
    auto conn1 = pool.borrow();
    assert(conn1.ok());

    const auto find_result = repo.findFriendship(1, 2);
    assert(find_result.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto create_result = repo.createFriendRequest(1, 2);
    assert(create_result.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto accept_result = repo.acceptFriendRequest(1, 2);
    assert(accept_result.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto delete_result = repo.deleteFriendship(1, 2);
    assert(delete_result.status == chat::RepositoryStatus::kBorrowTimeout);

    const auto list_result = repo.listFriendships(1);
    assert(list_result.status == chat::RepositoryStatus::kBorrowTimeout);
}

}  // namespace

int main() {
    TestReturnsUnavailableWhenConfigIncomplete();
    TestRepositoryMapsBorrowTimeout();
    std::cout << "[PASS] friend repository tests passed\n";
    return 0;
}
