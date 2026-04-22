#ifndef LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_
#define LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ConnectionMeta.h"
#include "IMessageHandler.h"
#include "codec/packet_codec.h"

// 本文件声明基于 epoll 的 TCP 服务器。
// 该类负责连接生命周期管理、事件循环以及收发缓冲协调。
// TODO(lzq): 将数据库访问等阻塞业务迁移到业务线程池。
// TODO(lzq): 为连接空闲超时、优雅停机和信号退出补充机制。

// 表示一个负责监听端口并分发客户端消息的 TCP 服务器。
class TcpServer
{
public:
  // 根据监听地址、端口和消息处理器构造服务器实例。
  TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler);

  // 析构时释放监听 socket、epoll 句柄和客户端连接资源。
  ~TcpServer();

  TcpServer(const TcpServer &) = delete;
  TcpServer &operator=(const TcpServer &) = delete;

  // 启动服务器并进入事件循环。
  bool start();

  // 停止服务器并关闭相关资源。
  void stop();

private:
  // 创建监听 socket。
  bool createListenSocket();

  // 将监听 socket 绑定到目标地址和端口。
  bool bindAddress();

  // 将监听 socket 切换为监听状态。
  bool startListen();

  // 运行 epoll 事件循环并处理连接事件。
  void acceptLoop(int epoll_fd);

  // 注册新连接并返回其服务端连接编号。
  uint64_t registerConnection(int conn_fd, const std::string &peer_ip,
                              uint16_t peer_port);

  // 在收到数据后更新连接活跃时间与接收统计。
  void touchOnRecv(uint64_t conn_id, size_t bytes);

  // 在发送数据后更新连接活跃时间与发送统计。
  void touchOnSend(uint64_t conn_id, size_t bytes);

  // 从连接表中注销连接并记录关闭原因。
  void unregisterConnection(uint64_t conn_id, const std::string &reason);

  // 输出一条连接元数据日志。
  void logConnectionMeta(const ConnectionMeta &meta);

  // 将指定文件描述符设置为非阻塞模式。
  bool set_nonblocking(int fd);

  // 关闭一个客户端连接并清理其状态。
  void closeClientFd(int fd, const std::string &reason);

  // 处理可读事件。
  void onReadable(int fd);

  // 根据文件描述符查找对应的连接编号。
  bool getConnIdByFd(int fd, uint64_t &conn_id);

  // 处理可写事件。
  void onWritable(int fd);

  // 向指定连接的待发送队列追加数据。
  bool appendPendingSend(int fd, const std::string &data);

  // 获取当前待发送缓冲区的连续可发送片段。
  bool peekPendingChunkCopy(int fd, std::string &out);

  // 判断指定连接是否仍有待发送数据。
  bool hasPendingSend(int fd);

  // 从待发送缓冲区中消费已经成功发送的字节。
  void consumePendingSend(int fd, size_t sent);

  // 为指定连接打开 EPOLLOUT 监听。
  bool enableWritableEvent(int fd);

  // 为指定连接关闭 EPOLLOUT 监听。
  bool disableWritableEvent(int fd);

  // 监听 socket 文件描述符。
  int listen_fd_;

  // epoll 实例文件描述符。
  int epoll_fd_;

  // 配置中的监听 IP。
  std::string ip_;

  // 配置中的监听端口。
  uint16_t port_;

  // 业务消息处理器引用。
  IMessageHandler &handler_;

  // 生成自增连接编号。
  std::atomic<uint64_t> next_conn_id_{1};

  // 保护连接元信息表。
  std::mutex connections_mutex_;

  // 保存所有活跃连接的元数据。
  std::unordered_map<uint64_t, ConnectionMeta> connections_;

  // 保护 fd 到 conn_id 的映射表。
  std::mutex fd_to_conn_id_mutex;

  // 保存运行时文件描述符到服务端连接编号的映射。
  std::unordered_map<int, uint64_t> fd_to_conn_id;

  // 保护待发送缓冲区映射表。
  std::mutex pending_send_mutex;

  // 保存每个连接待发送的应用层数据。
  std::unordered_map<int, std::string> pending_send_;

  // 保护每个连接的分包解码器实例。
  std::mutex packet_codecs_mutex;
  std::unordered_map<int, chat::PacketCodec> packet_codecs_;
};

#endif // LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_
