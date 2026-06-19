#ifndef LINUX_SERVER_INCLUDE_NET_TCP_CONNECTION_H_
#define LINUX_SERVER_INCLUDE_NET_TCP_CONNECTION_H_

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

// 本文件声明对已建立 TCP 连接的轻量封装。
// 它主要封装文件描述符生命周期和常见收发操作。
// TODO(lzq): 为非阻塞发送补充部分写入和错误码处理策略。
// TODO(lzq): 评估是否需要支持移动语义以便连接对象转移所有权。

// 表示一个已经建立的 TCP 客户端连接。
class TcpConnection {
   public:
    // 使用已建立连接的文件描述符和对端地址信息构造对象。
    TcpConnection(int conn_fd, const std::string& peer_ip, uint16_t peer_port);

    // 析构时关闭底层 socket。
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    TcpConnection(TcpConnection&&) noexcept;
    TcpConnection& operator=(TcpConnection&&) noexcept;

    // 从连接中接收字节流数据。
    ssize_t recv(char* buffer, size_t size);

    // 只调用一次send()
    ssize_t sendSome(const char* data, size_t len);

    // 关闭底层 socket。
    void close();

    // 放弃所有权，交出fd
    int releaseFd();

    // 关闭当前fd,接管新的fd
    void reset(int fd = -1);

    // 判断底层文件描述符是否有效。
    bool isValid() const;

    // 返回底层 socket 文件描述符。
    int fd() const;

    // 返回对端 IP 地址。
    std::string peerIp() const;

    // 返回对端端口号。
    uint16_t peerPort() const;

   private:
    // 已建立连接的 socket 文件描述符。
    int conn_fd_;

    // 对端 IP 地址。
    std::string peer_ip_;

    // 对端端口号。
    uint16_t peer_port_;
};

#endif  // LINUX_SERVER_INCLUDE_NET_TCP_CONNECTION_H_
