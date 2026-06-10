#ifndef LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_
#define LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "config/db_config.h"
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

// Redis 连接配置（暂未启用，结构预留）。
struct RedisConfig {
    bool enabled = false;

    std::string host = "127.0.0.1";
    uint16_t port = 6379;

    // 认证密码，留空表示不认证。
    std::string password;

    // 目标数据库索引。
    int database = 0;

    // 连接池大小。
    int pool_size = 4;

    // 连接超时（毫秒）。
    int connect_timeout_ms = 3000;

    // Key 前缀，避免与其他服务冲突。
    std::string key_prefix = "chat:";

    // 会话 Token TTL（秒）。
    int session_ttl_seconds = 86400;

    // 滑动窗口限流：窗口大小（秒）。
    int rate_limit_window_seconds = 60;

    // 滑动窗口限流：窗口内最大请求次数。
    int rate_limit_max_requests = 100;
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

// 服务器完整配置，由 ConfigLoader 构造并传递给 main()。
struct ServerConfig {
    ServerSection server;
    DbConfig mysql;
    DbPoolConfig mysql_pool;
    RedisConfig redis;
    LogConfig log;
    TimeoutConfig timeout;

    // 返回脱敏后的配置 JSON 字符串，可安全写入日志。
    // 密码字段一律替换为 "<redacted>"，不修改原始对象。
    std::string ToSafeString() const;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_CONFIG_SERVER_CONFIG_H_
