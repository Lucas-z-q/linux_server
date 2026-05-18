#ifndef LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_
#define LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_

#include <cstddef>
#include <cstdint>

namespace chat {

// 数据库连接池的高级行为配置
struct DbPoolConfig {
    // 最小连接数（池中保持的最少活动连接）
    std::size_t min_connections = 1;

    // 最大连接数（池的容量上限）
    std::size_t max_connections = 4;

    // 获取连接时的最大等待超时（毫秒）
    std::uint32_t borrow_timeout_ms = 1000;

    // 连接无效时的最大重试次数
    std::uint32_t connect_retry_count = 1;

    std::uint32_t connect_retry_delay_ms = 100;

    // 连接最大空闲时间（毫秒），超过后会被回收（注意：当前第一版暂未启用）
    std::uint32_t idle_timeout_ms = 300000;

    // 连接最大存活时间（毫秒），超过后即使在使用完毕也会被强制销毁重建（注意：当前第一版暂未启用）
    std::uint32_t max_lifetime_ms = 1800000;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_POOL_CONFIG_H_