#pragma once

#include <chrono>
#include <cstdint>
#include <string>

struct ConnectionMeta {
    uint64_t conn_id;  // server端唯一连接编号
    int fd;            // socket描述符

    std::string peer_ip;
    uint16_t peer_port;  // 远端地址。用于排查问题和审计。

    std::chrono::system_clock::time_point connected_at;    // 连接建立时间。用于计算在线时长。
    std::chrono::steady_clock::time_point last_active_at;  // 最近收/发时间。可用于超时踢连接。

    uint32_t recv_count;
    uint32_t send_count;  // 收发消息计数。用于流量监控和审计。

    uint64_t recv_bytes;
    uint64_t sent_bytes;  // 累计收发字节数。后续做流量监控有用。

    enum class State { CONNECTED, CLOSING, CLOSED } state;
};