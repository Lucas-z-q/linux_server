#ifndef LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_
#define LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

#include "ConnectionMeta.h"
#include "IMessageHandler.h"
#include "codec/packet_codec.h"
#include "concurrency/thread_pool.h"
#include "net/ConnectionContext.h"
#include "net/ResponseTask.h"
#include "stream/remote_push.h"

// 本文件声明基于 epoll 的 TCP 服务器。
// 该类负责连接生命周期管理、事件循环以及收发缓冲协调。
// TODO(lzq): 将数据库访问等阻塞业务迁移到业务线程池。
// TODO(lzq): 为连接空闲超时、优雅停机和信号退出补充机制。

// 表示一个负责监听端口并分发客户端消息的 TCP 服务器。
class TcpServer {
   public:
    // 根据监听地址、端口和消息处理器构造服务器实例。
    TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler);

    // 析构时释放监听 socket、epoll 句柄和客户端连接资源。
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    // 启动服务器并进入事件循环。
    bool start();

    // 请求停止服务器并唤醒事件循环，资源由事件循环退出后的清理路径释放。
    void stop();

    // 获取实际绑定的端口（如果配置了 0 端口，可通过此方法获取分配的端口）。
    uint16_t getPort() const { return port_.load(); }

    // Redis 消费线程通过此入口等待 I/O 线程完成连接校验和发送队列写入。
    chat::RemoteDeliveryOutcome deliverRemotePush(const chat::RemotePushEvent &event,
                                                  std::chrono::milliseconds timeout);

   private:
    static constexpr size_t kDefaultWorkerThreads = 4;

    struct RequestTask {
        std::weak_ptr<ConnectionContext> context;
        uint64_t conn_id;
        std::string peer_ip;
        std::string request;
    };

    struct PostDeliveryEvent {
        uint64_t conn_id = 0;
        bool finish_current_request = false;
    };

    struct RemotePushTask {
        chat::RemotePushEvent event;
        std::shared_ptr<std::promise<chat::RemoteDeliveryOutcome>> completion;
        std::shared_ptr<std::atomic<int>> state;
    };

    // 创建监听 socket。
    bool createListenSocket();

    // 将监听 socket 绑定到目标地址和端口。
    bool bindAddress();

    // 将监听 socket 切换为监听状态。
    bool startListen();

    // 运行 epoll 事件循环并处理连接事件。
    void acceptLoop(int epoll_fd);

    // 注册新连接并返回其连接上下文。
    std::shared_ptr<ConnectionContext> registerConnection(int conn_fd, const std::string &peer_ip, uint16_t peer_port);

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

    // 将请求任务投递到工作线程池。
    bool submitRequestTask(RequestTask task);

    // 完成当前请求，并按同连接顺序尝试投递下一个请求。
    // 该函数可在 worker 失败补偿路径调用，只能操作连接请求队列和线程池投递，不能做网络 I/O。
    bool finishCurrentRequestAndSubmitNext(const std::shared_ptr<ConnectionContext> &context, uint64_t conn_id);

    // 投递围栏解除后，如果连接空闲，则按 FIFO 投递队首请求。
    bool submitNextQueuedRequestIfIdle(const std::shared_ptr<ConnectionContext> &context, uint64_t conn_id);

    // 记录 markDelivered 完成事件，并唤醒 I/O 线程恢复请求推进。
    void enqueuePostDeliveryEvent(uint64_t conn_id, bool finish_current_request);

    // 将 worker 完成的响应任务放入 I/O 线程消费队列。
    bool enqueueResponseTask(ResponseTask task);

    // 处理 worker 结果唤醒事件。
    void onWorkerResultReadable();

    // 创建 worker 结果通知 eventfd。
    bool createWorkerEventFd();

    // 将 worker 结果通知 eventfd 注册到 epoll。
    bool registerWorkerEventFd();

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

    // worker 完成任务后唤醒 I/O 线程的 eventfd。
    int worker_event_fd_;
    std::mutex worker_event_fd_mutex_;

    // 配置中的监听 IP。
    std::string ip_;

    // 配置中的监听端口；传入 0 时，绑定后保存系统分配的真实端口。
    std::atomic<uint16_t> port_;

    // 业务消息处理器引用。
    IMessageHandler &handler_;

    // 用于后续投递业务请求处理任务的工作线程池。
    ThreadPool worker_pool_;

    // worker 已完成响应任务队列，由 I/O 线程消费。
    std::mutex completed_tasks_mutex_;
    std::queue<ResponseTask> completed_tasks_;
    std::atomic<uint64_t> next_response_task_id_{1};

    std::mutex remote_push_tasks_mutex_;
    std::vector<RemotePushTask> remote_push_tasks_;

    // 标记服务器正在停止，停止后拒绝新的请求任务投递。
    std::atomic<bool> stopping_{false};

    // 生成自增连接编号。
    std::atomic<uint64_t> next_conn_id_{1};

    // 保护 conn_id 到 ConnectionContext 的映射表。
    std::mutex connections_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<ConnectionContext>> connections_;  // 保存所有活跃连接的上下文

    // 保护 fd 到 ConnectionContext 的映射表。
    std::mutex fd_to_context_mutex_;
    std::unordered_map<int, std::shared_ptr<ConnectionContext>> fd_to_context_;  // fd 到 ConnectionContext 的索引

    // 辅助函数：通过fd查找ConnectionContext
    std::shared_ptr<ConnectionContext> getConnectionContextByFd(int fd);

    // 辅助函数：通过连接ID查找ConnectionContext
    std::shared_ptr<ConnectionContext> getConnectionContextById(chat::ConnectionId conn_id);

    // 辅助函数：日志记录连接断开状态
    void logConnectionDisconnected(const ConnectionMeta &meta, const std::string &reason);

    // worker 线程完成 markDelivered 后回信的连接恢复事件队列。
    std::mutex post_delivery_mutex_;
    std::vector<PostDeliveryEvent> post_delivery_events_;

    // 向 worker_event_fd_ 写入通知，唤醒 I/O 线程。
    void notifyWorkerEventFd();
};

#endif  // LINUX_SERVER_INCLUDE_NET_TCP_SERVER_H_
