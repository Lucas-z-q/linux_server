#ifndef LINUX_SERVER_INCLUDE_NET_CONNECTION_META_H_
#define LINUX_SERVER_INCLUDE_NET_CONNECTION_META_H_

#include <chrono>
#include <cstdint>
#include <string>

// 本文件定义服务端维护的连接元信息。
// 这些字段主要用于连接管理、审计、统计以及后续登录态扩展。
//
// TODO(lzq): 将认证相关字段并入本结构或关联到 ConnectionSession。
// TODO(lzq): 增加入站缓冲和协议状态字段，便于处理半包。
// TODO(lzq): 明确状态迁移图，防止连接关闭路径分叉过多。

// 表示一个活跃 TCP 连接在服务端侧的运行时元数据。
struct ConnectionMeta {
    // 服务端分配的唯一连接编号。
    uint64_t conn_id;

    // 对应的 socket 文件描述符。
    int fd;

    // 对端 IP 地址。
    std::string peer_ip;

    // 对端端口号。
    uint16_t peer_port;

    // 连接建立时间，用于计算会话生命周期。
    std::chrono::system_clock::time_point connected_at;

    // 最近一次读写发生的时间，用于空闲超时判断。
    std::chrono::steady_clock::time_point last_active_at;

    // 累计接收的消息数。
    uint32_t recv_count;

    // 累计发送的消息数。
    uint32_t send_count;

    // 累计接收的字节数。
    uint64_t recv_bytes;

    // 累计发送的字节数。
    uint64_t sent_bytes;

    // 连接在服务端生命周期中的状态。
    enum class State { CONNECTED, CLOSING, CLOSED } state;
};

#endif  // LINUX_SERVER_INCLUDE_NET_CONNECTION_META_H_
