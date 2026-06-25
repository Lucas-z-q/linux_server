#ifndef LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_
#define LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

// DbConfig 描述单个 MySQL 连接和连接池创建连接时使用的基础参数。

namespace chat {

// 描述 MySQL 连接或连接池初始化所需的基础参数。
struct DbConfig {
    // 数据库主机地址。
    std::string host = "127.0.0.1";

    // 数据库服务端口。
    std::uint16_t port = 3306;

    // 登录数据库的用户名。
    std::string username;

    // 登录数据库的密码。
    std::string password;

    // 目标数据库名称。
    std::string database;

    // 建立连接的超时时间，单位秒。
    std::uint32_t connect_timeout_seconds = 5;

    // 读操作超时时间，单位秒。
    std::uint32_t read_timeout_seconds = 5;

    // 写操作超时时间，单位秒。
    std::uint32_t write_timeout_seconds = 5;

    // 连接使用的字符集。
    std::string charset = "utf8mb4";
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_
