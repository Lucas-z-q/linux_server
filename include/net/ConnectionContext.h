#ifndef LINUX_SERVER_INCLUDE_NET_CONNECTION_CONTEXT_H_
#define LINUX_SERVER_INCLUDE_NET_CONNECTION_CONTEXT_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "codec/packet_codec.h"
#include "common/types.h"
#include "net/ConnectionMeta.h"
#include "net/TcpConnection.h"

// 表示单个网络连接的上下文和状态。
// 它封装了文件描述符、连接 ID、元数据、
// 应用层待发送缓冲区，以及用于处理输入数据帧的数据包编解码器。
class ConnectionContext {
   private:
    TcpConnection connection_;
    chat::ConnectionId conn_id_;  // 此连接的唯一标识符。
    std::string pending_send_;    // 等待通过套接字发送的数据缓冲区。

    chat::PacketCodec packet_codec_;  // 用于将连续字节流解析为独立数据包的编解码器。
    ConnectionMeta meta_;             // 连接的元数据和统计信息。

    std::mutex request_mutex_;                  // 保护同连接请求串行化状态。
    bool request_in_flight_;                    // 是否已有请求正在 worker 中处理。
    std::queue<std::string> pending_requests_;  // 等待按连接顺序处理的请求队列。

   public:
    // 构造一个新的 ConnectionContext。
    // 使用给定的对端信息初始化元数据，并将当前时间设置为连接时间和最后活跃时间戳。
    ConnectionContext(TcpConnection conn, chat::ConnectionId conn_id);
    ~ConnectionContext() = default;

    // 返回底层的套接字文件描述符。
    int fd() const;

    // 返回唯一的连接 ID。
    chat::ConnectionId conn_id() const;

    // 返回连接元数据的只读引用。
    const ConnectionMeta& meta() const;

    // 从底层连接接收字节流数据。
    ssize_t recv(char* buffer, size_t size);

    // 向底层连接尝试发送一段数据，允许部分写入。
    ssize_t sendSome(const char* data, size_t len);

    // 关闭底层连接。
    void closeConnection();

    // 将一块原始字节数据送入数据包编解码器。
    // 从数据块中解析出的完整数据包将追加到 `packets` 中。
    // 如果数据处理成功返回 true，发生编解码错误返回 false。
    bool feedPacketData(const std::string& chunk, std::vector<std::string>& packets);

    // 将数据追加到待发送缓冲区中。
    void appendPendingSend(const std::string& data);

    // 检查是否有等待发送的数据。
    // 如果待发送缓冲区不为空，则返回 true。
    bool hasPendingSend() const;

    // 查看待发送缓冲区中的数据但不消费它。
    // 将待发送数据复制到 `out` 中。如果缓冲区为空则返回 false。
    bool peekPendingSend(std::string& out) const;

    // 从待发送缓冲区的头部消费指定数量的字节。
    // 通常在成功通过套接字发送数据后调用。
    void consumePendingSend(size_t bytes);

    // 清空待发送缓冲区中的所有数据。
    void clearPendingSend();

    // 如果当前连接没有正在处理的请求，则立即占用执行权并返回 true；否则将请求排队。
    bool startRequestOrQueue(const std::string& request);

    // 标记当前请求完成，并取出同连接的下一个待处理请求。
    bool finishRequestAndPopNext(std::string& next_request);

    // 清空当前连接尚未处理的请求。
    void clearPendingRequests();

    // 在接收到数据后更新元数据。
    // 递增接收次数，并将 `bytes` 累加到接收总字节数中。
    // 同时更新最后活跃时间戳。
    void touchOnRecv(size_t bytes);

    // 在发送数据后更新元数据。
    // 递增发送次数，并将 `bytes` 累加到发送总字节数中。
    // 同时更新最后活跃时间戳。
    void touchOnSend(size_t bytes);

    // 将连接状态标记为 CLOSING（正在关闭）。
    void markClosing();

    // 将连接状态标记为 CLOSED（已关闭）。
    void markClosed();
};

#endif  // LINUX_SERVER_INCLUDE_NET_CONNECTION_CONTEXT_H_
