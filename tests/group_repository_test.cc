#include "db/group_repository.h"

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

chat::GroupRecord MakeGroup() {
    return {.id = "grp_test", .name = "test", .owner_id = 1, .conversation_id = "gconv_grp_test"};
}

std::vector<chat::GroupMemberRecord> MakeMembers() {
    return {
        {.group_id = "grp_test", .user_id = 1, .role = "owner"},
        {.group_id = "grp_test", .user_id = 2, .role = "member"},
    };
}

void TestReturnsUnavailableWhenConfigIncomplete() {
    chat::DbConfig config;
    config.host = "127.0.0.1";
    config.port = 3307;
    config.username = "";
    config.password = "";
    config.database = "";

    chat::DbPool pool(config);
    chat::GroupRepository repo(&pool);

    assert(repo.createGroup(MakeGroup(), MakeMembers()).status == chat::RepositoryStatus::kConnectionUnavailable);
    assert(repo.findGroup("grp_test").status == chat::RepositoryStatus::kConnectionUnavailable);
    assert(repo.addMember({.group_id = "grp_test", .user_id = 3, .role = "member"}).status ==
           chat::RepositoryStatus::kConnectionUnavailable);
    assert(repo.listMembers("grp_test").status == chat::RepositoryStatus::kConnectionUnavailable);
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
    chat::GroupRepository repo(&pool);

    auto conn = pool.borrow();
    assert(conn.ok());

    assert(repo.createGroup(MakeGroup(), MakeMembers()).status == chat::RepositoryStatus::kBorrowTimeout);
    assert(repo.findGroup("grp_test").status == chat::RepositoryStatus::kBorrowTimeout);
    assert(repo.addMember({.group_id = "grp_test", .user_id = 3, .role = "member"}).status ==
           chat::RepositoryStatus::kBorrowTimeout);
    assert(repo.listMembers("grp_test").status == chat::RepositoryStatus::kBorrowTimeout);
}

}  // namespace

int main() {
    TestReturnsUnavailableWhenConfigIncomplete();
    TestRepositoryMapsBorrowTimeout();
    std::cout << "[PASS] group repository tests passed\n";
    return 0;
}
