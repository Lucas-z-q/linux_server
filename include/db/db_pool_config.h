#ifndef LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_
#define LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace chat {

// 数据库连接池的高级行为配置
struct DbPoolConfig {
    // 最小连接数（池中保持的最少活动连接）
    std::size_t min_connections = 2;

    // 最大连接数（池的容量上限）
    std::size_t max_connections = 10;

    // 获取连接时的最大等待超时（毫秒）
    std::uint32_t borrow_timeout_ms = 5000;

    // 连接无效时的最大重试次数（注意：当前第一版暂未启用）
    std::uint32_t retry_count = 3;

    // 连接最大空闲时间（毫秒），超过后会被回收（注意：当前第一版暂未启用）
    std::uint32_t idle_timeout_ms = 600000;

    // 连接最大存活时间（毫秒），超过后即使在使用完毕也会被强制销毁重建（注意：当前第一版暂未启用）
    std::uint32_t max_lifetime_ms = 3600000;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_