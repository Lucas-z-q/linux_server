#include "db/db_connection.h"

#include <gtest/gtest.h>

#include <utility>

#include "config/db_config.h"

namespace chat {
namespace {

TEST(DbConnectionTest, InitAndMoveSemantics) {
    DbConfig config;
    DbConnection conn(config);

    // 初始状态应该为未连接
    EXPECT_FALSE(conn.isConnected());
    EXPECT_EQ(conn.nativeHandle(), nullptr);

    // 测试移动构造，确保资源管理正确
    DbConnection conn2(std::move(conn));
    EXPECT_FALSE(conn2.isConnected());
}

}  // namespace
}  // namespace chat