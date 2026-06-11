#ifndef LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_
#define LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/db_config.h"
#include "config/redis_config.h"
#include "db/db_pool_config.h"

// 本文件定义服务器完整配置的聚合结构。
// 所有子系统配置均通过 ServerConfig 统一传递，不再散落在 main() 中逐一构造。

namespace chat {

// 网络监听段配置。
struct ServerSection {
    // 监听 IP，必须能被 inet_pton() 解析。
    std::string listen_ip = "127.0.0.1";

    // 监听端口，范围 1..65535。
    uint16_t listen_port = 8080;
};

// 日志配置。
struct LogConfig {
    // 日志级别：debug / info / warn / error。
    std::string level = "info";

    // 日志文件路径。
    std::string file_path = "logs/server.log";

    // 是否同时输出到控制台。
    bool console = true;

    // 单个日志文件的最大大小，单位 MB。
    std::size_t max_size_mb = 100;

    // 最多保留的文件数，包含当前日志文件。
    std::size_t max_files = 5;

    // 是否启用独立后台线程异步写入。
    bool async = true;
};

// 超时配置。
struct TimeoutConfig {
    // 远程推送超时（毫秒）。
    uint32_t remote_push_ms = 500;
};

// 未认证连接的空闲超时配置。
struct ConnectionConfig {
    uint32_t idle_timeout_ms = 300000;
};

// 已认证连接的心跳超时配置。
struct HeartbeatConfig {
    uint32_t timeout_ms = 90000;
};

// 服务器完整配置，由 ConfigLoader 构造并传递给 main()。
struct ServerConfig {
    ServerSection server;
    DbConfig mysql;
    DbPoolConfig mysql_pool;
    RedisConfig redis;
    LogConfig log;
    TimeoutConfig timeout;
    ConnectionConfig connection;
    HeartbeatConfig heartbeat;

    // 返回脱敏后的配置 JSON 字符串，可安全写入日志。
    // 密码字段一律替换为 "<redacted>"，不修改原始对象。
    std::string ToSafeString() const;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_
