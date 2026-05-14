#ifndef LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_
#define LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

// 本文件定义数据库连接相关配置项。
// 该配置结构既可用于单连接初始化，也可用于后续连接池扩展。
//
// TODO(lzq): 增加从配置文件或环境变量加载的能力。
// TODO(lzq): 对敏感字段的日志输出增加脱敏规范。

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

  // 连接池计划持有的连接数量。
  std::size_t pool_size = 4;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_DB_CONFIG_H_
