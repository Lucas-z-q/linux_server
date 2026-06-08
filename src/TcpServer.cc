#include "net/TcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <queue>
#include <sstream>
#include <utility>
#include <vector>

/**
 * @file TcpServer.cc
 * @brief Implements TcpServer lifecycle with epoll-based read/write event handling.
 */

TcpServer::TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler)
    : listen_fd_(-1),
      epoll_fd_(-1),
      worker_event_fd_(-1),
      ip_(ip),
      port_(port),
      handler_(handler),
      worker_pool_(kDefaultWorkerThreads),
      stopping_(false) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::createListenSocket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        return false;
    }
    return true;
}

bool TcpServer::bindAddress() {
    const uint16_t requested_port = port_.load();

    // 尝试 IPv4，失败时尝试 IPv6，使配置中的 IP 真正生效。
    struct sockaddr_in addr4;
    memset(&addr4, 0, sizeof(addr4));
    addr4.sin_family = AF_INET;
    addr4.sin_port = htons(requested_port);

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(requested_port);

    struct sockaddr* bind_addr = nullptr;
    socklen_t bind_len = 0;

    if (inet_pton(AF_INET, ip_.c_str(), &addr4.sin_addr) == 1) {
        bind_addr = reinterpret_cast<struct sockaddr*>(&addr4);
        bind_len = sizeof(addr4);
    } else if (inet_pton(AF_INET6, ip_.c_str(), &addr6.sin6_addr) == 1) {
        bind_addr = reinterpret_cast<struct sockaddr*>(&addr6);
        bind_len = sizeof(addr6);
    } else {
        std::cerr << "bindAddress: invalid IP address: " << ip_ << std::endl;
        return false;
    }

    if (bind(listen_fd_, bind_addr, bind_len) < 0) {
        perror("Bind failed.");
        return false;
    }

    if (requested_port == 0) {
        socklen_t len = bind_len;
        if (getsockname(listen_fd_, bind_addr, &len) == 0) {
            if (bind_addr->sa_family == AF_INET) {
                port_.store(ntohs(reinterpret_cast<struct sockaddr_in*>(bind_addr)->sin_port));
            } else {
                port_.store(ntohs(reinterpret_cast<struct sockaddr_in6*>(bind_addr)->sin6_port));
            }
        }
    }
    return true;
}

bool TcpServer::startListen() {
    if (listen(listen_fd_, 5) < 0) {
        perror("Listen failed.");
        return false;
    }
    return true;
}

void TcpServer::stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
        return;
    }

    // 主动写入 eventfd，安全唤醒 acceptLoop 中的 epoll_wait。
    {
        std::lock_guard<std::mutex> lock(worker_event_fd_mutex_);
        if (worker_event_fd_ != -1) {
            const uint64_t notify_value = 1;
            if (write(worker_event_fd_, &notify_value, sizeof(notify_value)) < 0 && errno != EAGAIN) {
                // 忽略错误，因为服务即将关停
            }
        }
    }
}

bool TcpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL) failed");
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL) failed");
        return false;
    }
    return true;
}

bool TcpServer::start() {
    if (stopping_.load())
        return false;

    auto cleanup_resources = [this]() {
        stop();               // 确保安全设置了停止标志
        worker_pool_.stop();  // 阻塞等待业务线程安全降落

        if (listen_fd_ != -1) {
            close(listen_fd_);
            listen_fd_ = -1;
        }

        std::vector<int> client_fds;
        {
            std::lock_guard<std::mutex> lock(fd_to_context_mutex_);
            client_fds.reserve(fd_to_context_.size());
            for (const auto &entry : fd_to_context_) {
                client_fds.push_back(entry.first);
            }
        }
        for (int fd : client_fds) {
            closeClientFd(fd, "server_stop");
        }

        {
            std::lock_guard<std::mutex> lock(completed_tasks_mutex_);
            std::queue<ResponseTask> empty;
            completed_tasks_.swap(empty);
        }

        {
            std::lock_guard<std::mutex> lock(worker_event_fd_mutex_);
            if (worker_event_fd_ != -1) {
                close(worker_event_fd_);
                worker_event_fd_ = -1;
            }
        }
        if (epoll_fd_ != -1) {
            close(epoll_fd_);
            epoll_fd_ = -1;
        }
    };

    if (!createListenSocket()) {
        cleanup_resources();
        return false;
    }
    if (!bindAddress()) {
        cleanup_resources();
        return false;
    }
    if (!startListen()) {
        cleanup_resources();
        return false;
    }

    // EPOLLET requires non-blocking sockets, otherwise edge-trigger behavior is unsafe.
    if (!set_nonblocking(listen_fd_)) {
        cleanup_resources();
        return false;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        perror("epoll_create failed.");
        cleanup_resources();
        return false;
    }

    if (!createWorkerEventFd() || !registerWorkerEventFd()) {
        cleanup_resources();
        return false;
    }

    // Register the listen fd to receive incoming-connection notifications.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        perror("epoll_ctl add listen_fd failed");
        cleanup_resources();
        return false;
    }

    acceptLoop(epoll_fd_);
    cleanup_resources();
    return true;
}

void TcpServer::acceptLoop(int epoll_fd) {
    constexpr int kMaxEvents = 1024;
    struct epoll_event events[kMaxEvents];
    struct sockaddr_in client_addr;

    while (!stopping_.load()) {
        int n = epoll_wait(epoll_fd, events, kMaxEvents, -1);
        if (stopping_.load()) {
            break;  // 被唤醒后发现已停机，直接跳出循环避免处理后续事件
        }
        if (n < 0) {
            // Signal interruption is transient.
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t event_mask = events[i].events;

            if (fd == worker_event_fd_) {
                onWorkerResultReadable();
                continue;
            }

            // For connected sockets, handle close/error/read/write events.
            if (fd != listen_fd_) {
                // Hard errors are handled first.
                if (event_mask & EPOLLERR) {
                    closeClientFd(fd, "peer_error");
                    continue;
                }

                // Read/write first, so half-close peers can still receive queued responses.
                if (event_mask & EPOLLIN) {
                    onReadable(fd);
                }
                if (event_mask & EPOLLOUT) {
                    onWritable(fd);
                }
                // After I/O handling, close only when peer is closed and no data remains to send.
                if (event_mask & (EPOLLHUP | EPOLLRDHUP)) {
                    if (!hasPendingSend(fd)) {
                        closeClientFd(fd, "peer_hup_or_error");
                    }
                }
                continue;
            }

            // listen_fd_ is readable: accept all pending connections.
            while (true) {
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd_, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("Accept failed");
                    stop();
                    return;
                }

                if (!set_nonblocking(client_fd)) {
                    close(client_fd);
                    continue;
                }

                // New connections start in read-only interest; EPOLLOUT is enabled on demand.
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
                    perror("epoll_ctl add client_fd failed");
                    close(client_fd);
                    continue;
                }

                registerConnection(client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            }
        }
    }
}

std::shared_ptr<ConnectionContext> TcpServer::registerConnection(int conn_fd, const std::string &peer_ip,
                                                                 uint16_t peer_port) {
    const uint64_t conn_id = next_conn_id_.fetch_add(1);

    TcpConnection conn(conn_fd, peer_ip, peer_port);
    auto context = std::make_shared<ConnectionContext>(std::move(conn), conn_id);

    {
        std::scoped_lock lock(connections_mutex_, fd_to_context_mutex_);
        connections_[conn_id] = context;
        fd_to_context_[conn_fd] = context;
    }

    logConnectionMeta(context->meta());
    return context;
}

void TcpServer::unregisterConnection(uint64_t conn_id, const std::string &reason) {
    ConnectionMeta snapshot{};
    std::shared_ptr<ConnectionContext> context;
    {
        // Remove once under lock to keep close path idempotent.
        std::scoped_lock lock(connections_mutex_, fd_to_context_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end()) {
            return;
        }
        context = it->second;
        context->markClosing();
        fd_to_context_.erase(context->fd());
        connections_.erase(it);
    }

    context->markClosed();
    snapshot = context->meta();
    logConnectionDisconnected(snapshot, reason);
}

void TcpServer::logConnectionMeta(const ConnectionMeta &meta) {
    const auto now = std::chrono::system_clock::to_time_t(meta.connected_at);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%F %T");

    std::cout << "[connected] conn_id=" << meta.conn_id << " peer=" << meta.peer_ip << ":" << meta.peer_port
              << " fd=" << meta.fd << " at=" << oss.str() << std::endl;
}

void TcpServer::closeClientFd(int fd, const std::string &reason) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        return;
    }
    const uint64_t conn_id = context->conn_id();

    // DEL may fail if the fd was already removed/closed by another path.
    if (epoll_fd_ != -1 && epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT && errno != EBADF) {
        perror("epoll_ctl del client_fd failed");
    }

    context->clearPendingSend();
    context->clearPendingRequests();
    handler_.onConnectionClosed(conn_id);
    unregisterConnection(conn_id, reason);
    context->closeConnection();
}

void TcpServer::onReadable(int fd) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        std::cerr << "can't find connection context for fd=" << fd << std::endl;
        return;
    }
    const uint64_t conn_id = context->conn_id();

    char buff[1024];
    while (true) {
        memset(buff, 0, sizeof(buff));
        ssize_t n = context->recv(buff, sizeof(buff) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else {
                closeClientFd(fd, "recv_error");
                return;
            }
        }
        if (n == 0) {
            // Peer half-closed. Keep socket alive until pending response is drained.
            break;
        }

        context->touchOnRecv(static_cast<size_t>(n));
        std::cout << "[message_received] conn_id=" << conn_id << " bytes=" << n << std::endl;
        std::string chunk(buff, static_cast<size_t>(n));

        std::vector<std::string> packets;
        if (!context->feedPacketData(chunk, packets)) {
            closeClientFd(fd, "packet_too_large");
            return;
        }

        for (const std::string &packet : packets) {
            if (packet.empty()) {
                continue;
            }

            RequestTask task{std::weak_ptr<ConnectionContext>(context), conn_id, packet};
            if (context->startRequestOrQueue(packet) && !submitRequestTask(std::move(task))) {
                closeClientFd(fd, "submit_request_task_failed");
                return;
            }
        }
    }
}

bool TcpServer::submitRequestTask(RequestTask task) {
    if (stopping_.load()) {
        return false;
    }

    try {
        worker_pool_.submit([this, task = std::move(task)]() mutable {
            try {
                // TODO(lzq): Some handlers, such as login, mutate session state before
                // the I/O thread can revalidate that the connection is still alive.
                // A stricter phase should move those side effects behind I/O-thread validation.
                HandleResult result = handler_.handle(task.request, task.conn_id);

                auto context = task.context.lock();
                if (!context) {
                    return;
                }

                ResponseTask response_task{std::weak_ptr<ConnectionContext>(context),
                                           task.conn_id,
                                           std::move(result.response),
                                           next_response_task_id_.fetch_add(1),
                                           false,
                                           result.session_action,
                                           std::move(result.pending_session),
                                           std::move(result.pushes),
                                           std::move(result.delivered_message_ids),
                                           result.delivered_user_id};
                enqueueResponseTask(std::move(response_task));
            } catch (const std::exception &ex) {
                std::cerr << "worker request failed: " << ex.what() << std::endl;
                auto context = task.context.lock();
                if (context) {
                    ResponseTask close_task{std::weak_ptr<ConnectionContext>(context), task.conn_id, "",
                                            next_response_task_id_.fetch_add(1), true};
                    if (!enqueueResponseTask(std::move(close_task))) {
                        context->clearPendingRequests();
                    }
                }
            } catch (...) {
                std::cerr << "worker request failed: unknown exception" << std::endl;
                auto context = task.context.lock();
                if (context) {
                    ResponseTask close_task{std::weak_ptr<ConnectionContext>(context), task.conn_id, "",
                                            next_response_task_id_.fetch_add(1), true};
                    if (!enqueueResponseTask(std::move(close_task))) {
                        context->clearPendingRequests();
                    }
                }
            }
        });
    } catch (const std::exception &ex) {
        std::cerr << "submit request task failed: " << ex.what() << std::endl;
        return false;
    }
    return true;
}

bool TcpServer::finishCurrentRequestAndSubmitNext(const std::shared_ptr<ConnectionContext> &context, uint64_t conn_id) {
    std::string next_request;
    if (!context->finishRequestAndPopNext(next_request)) {
        return true;
    }

    RequestTask next_task{std::weak_ptr<ConnectionContext>(context), conn_id, std::move(next_request)};
    if (submitRequestTask(std::move(next_task))) {
        return true;
    }

    context->clearPendingRequests();
    return false;
}

bool TcpServer::submitNextQueuedRequestIfIdle(const std::shared_ptr<ConnectionContext> &context, uint64_t conn_id) {
    std::string next_request;
    if (!context->popNextRequestIfIdle(next_request)) {
        return true;
    }

    RequestTask next_task{std::weak_ptr<ConnectionContext>(context), conn_id, std::move(next_request)};
    if (submitRequestTask(std::move(next_task))) {
        return true;
    }

    context->clearPendingRequests();
    return false;
}

void TcpServer::enqueuePostDeliveryEvent(uint64_t conn_id, bool finish_current_request) {
    {
        std::lock_guard<std::mutex> lock(post_delivery_mutex_);
        post_delivery_events_.push_back({conn_id, finish_current_request});
    }
    notifyWorkerEventFd();
}

bool TcpServer::enqueueResponseTask(ResponseTask task) {
    if (stopping_.load()) {
        return false;
    }

    const uint64_t sequence = task.sequence;

    std::lock_guard<std::mutex> fd_lock(worker_event_fd_mutex_);
    if (worker_event_fd_ == -1) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(completed_tasks_mutex_);
        completed_tasks_.push(std::move(task));
    }

    while (true) {
        const uint64_t notify_value = 1;
        ssize_t n = write(worker_event_fd_, &notify_value, sizeof(notify_value));
        if (n == sizeof(notify_value)) {
            return true;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        if (n < 0) {
            perror("write worker_event_fd failed (critical)");
            // 即使 write 失败，任务已经在队列里了。
            // I/O 线程最终会在下一次 eventfd 触发或轮询时排空队列。
            // 不要去扫描并删除任务！
        }
        return true;  // 依然返回 true，告诉调用者任务已成功交接给 I/O 队列
    }
}

void TcpServer::notifyWorkerEventFd() {
    const uint64_t notify_value = 1;
    while (true) {
        ssize_t n = write(worker_event_fd_, &notify_value, sizeof(notify_value));
        if (n == sizeof(notify_value)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN/EWOULDBLOCK 表示 eventfd 已有未消费事件，其他错误不影响正确性
        return;
    }
}

void TcpServer::onWorkerResultReadable() {
    uint64_t event_count = 0;
    {
        std::lock_guard<std::mutex> lock(worker_event_fd_mutex_);
        if (worker_event_fd_ != -1) {
            while (true) {
                ssize_t n = read(worker_event_fd_, &event_count, sizeof(event_count));
                if (n == sizeof(event_count)) {
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }
                if (n < 0) {
                    perror("read worker_event_fd failed");
                }
                break;
            }
        }
    }

    std::queue<ResponseTask> ready_tasks;
    {
        std::lock_guard<std::mutex> lock(completed_tasks_mutex_);
        ready_tasks.swap(completed_tasks_);
    }

    while (!ready_tasks.empty()) {
        ResponseTask task = std::move(ready_tasks.front());
        ready_tasks.pop();

        auto context = task.context.lock();
        if (!context) {
            continue;
        }

        if (task.close_after) {
            // closeClientFd clears both the current in-flight request and queued pending requests.
            closeClientFd(context->fd(), "worker_request_exception");
            continue;
        }

        bool deferred_next_request = false;

        if (!task.response.empty()) {
            chat::PacketCodec codec;
            context->appendPendingSend(codec.encode(task.response));
            if (!enableWritableEvent(context->fd())) {
                closeClientFd(context->fd(), "enable_write_event_failed");
                continue;
            }
            if (!task.delivered_message_ids.empty()) {
                std::vector<std::string> msg_ids = std::move(task.delivered_message_ids);
                chat::UserId user_id = task.delivered_user_id;
                uint64_t conn_id = task.conn_id;
                try {
                    worker_pool_.submit([this, user_id, msg_ids = std::move(msg_ids), conn_id]() {
                        try {
                            handler_.onMessagesDelivered(user_id, msg_ids);
                        } catch (const std::exception &ex) {
                            std::cerr << "markDelivered batch failed: " << ex.what() << std::endl;
                        } catch (...) {
                            std::cerr << "markDelivered batch failed: unknown exception" << std::endl;
                        }
                        enqueuePostDeliveryEvent(conn_id, true);
                    });
                    deferred_next_request = true;
                } catch (const std::exception &ex) {
                    std::cerr << "submit markDelivered batch failed: " << ex.what() << std::endl;
                } catch (...) {
                    std::cerr << "submit markDelivered batch failed: unknown exception" << std::endl;
                }
            }
        }

        if (task.session_action == SessionAction::BIND) {
            handler_.applyBindSession(task.conn_id, task.pending_session);
        } else if (task.session_action == SessionAction::UNBIND) {
            handler_.applyUnbindSession(task.conn_id);
        }

        // 处理主动推送消息 (pushes)
        std::vector<std::string> same_conn_push_msg_ids;
        chat::UserId same_conn_user_id = 0;
        std::unordered_map<uint64_t, std::pair<std::vector<std::string>, chat::UserId>> cross_conn_pushes;
        for (const auto &push : task.pushes) {
            // 校验目标连接当前仍属于原接收用户，避免在连接复用/重新登录时误投递
            if (!handler_.isConnectionBoundToUser(push.target_conn_id, push.target_user_id)) {
                continue;
            }

            std::shared_ptr<ConnectionContext> target_context = getConnectionContextById(push.target_conn_id);

            if (target_context && !push.payload.empty()) {
                chat::PacketCodec codec;
                target_context->appendPendingSend(codec.encode(push.payload));
                if (!enableWritableEvent(target_context->fd())) {
                    closeClientFd(target_context->fd(), "enable_write_event_failed");
                } else if (!push.message_id.empty()) {
                    if (push.target_conn_id == task.conn_id) {
                        same_conn_push_msg_ids.push_back(push.message_id);
                        if (same_conn_user_id == 0) {
                            same_conn_user_id = push.target_user_id;
                        }
                    } else {
                        auto &entry = cross_conn_pushes[push.target_conn_id];
                        entry.first.push_back(push.message_id);
                        if (entry.second == 0) {
                            entry.second = push.target_user_id;
                        }
                    }
                }
            }
        }

        // 处理跨连接推送的投递标记：对目标连接加围栏，串行化后续请求直至 DB 更新完成
        for (auto &[target_conn_id, entry] : cross_conn_pushes) {
            auto target_ctx = getConnectionContextById(target_conn_id);
            if (!target_ctx)
                continue;

            target_ctx->incrementPendingDeliveryMarks();

            std::vector<std::string> msg_ids = std::move(entry.first);
            chat::UserId user_id = entry.second;
            try {
                worker_pool_.submit([this, user_id, msg_ids = std::move(msg_ids), target_conn_id]() {
                    try {
                        handler_.onMessagesDelivered(user_id, msg_ids);
                    } catch (const std::exception &ex) {
                        std::cerr << "markDelivered cross-conn push failed: " << ex.what() << std::endl;
                    } catch (...) {
                        std::cerr << "markDelivered cross-conn push failed: unknown exception" << std::endl;
                    }
                    auto ctx = getConnectionContextById(target_conn_id);
                    if (ctx) {
                        ctx->decrementPendingDeliveryMarks();
                        enqueuePostDeliveryEvent(target_conn_id, false);
                    }
                });
            } catch (const std::exception &ex) {
                target_ctx->decrementPendingDeliveryMarks();
                std::cerr << "submit markDelivered cross-conn push failed: " << ex.what() << std::endl;
                enqueuePostDeliveryEvent(target_conn_id, false);
            } catch (...) {
                target_ctx->decrementPendingDeliveryMarks();
                std::cerr << "submit markDelivered cross-conn push failed: unknown exception" << std::endl;
                enqueuePostDeliveryEvent(target_conn_id, false);
            }
        }

        if (!same_conn_push_msg_ids.empty()) {
            uint64_t conn_id = task.conn_id;
            chat::UserId user_id = same_conn_user_id;
            try {
                worker_pool_.submit([this, user_id, msg_ids = std::move(same_conn_push_msg_ids), conn_id]() {
                    try {
                        handler_.onMessagesDelivered(user_id, msg_ids);
                    } catch (const std::exception &ex) {
                        std::cerr << "markDelivered same-conn push failed: " << ex.what() << std::endl;
                    } catch (...) {
                        std::cerr << "markDelivered same-conn push failed: unknown exception" << std::endl;
                    }
                    enqueuePostDeliveryEvent(conn_id, true);
                });
                deferred_next_request = true;
            } catch (const std::exception &ex) {
                std::cerr << "submit markDelivered same-conn push failed: " << ex.what() << std::endl;
            } catch (...) {
                std::cerr << "submit markDelivered same-conn push failed: unknown exception" << std::endl;
            }
        }

        if (!deferred_next_request) {
            if (!finishCurrentRequestAndSubmitNext(context, task.conn_id)) {
                closeClientFd(context->fd(), "submit_request_task_failed");
            }
        }
    }

    // 处理 markDelivered 已完成的连接，按事件类型恢复请求提交。
    std::vector<PostDeliveryEvent> ready_events;
    {
        std::lock_guard<std::mutex> lock(post_delivery_mutex_);
        ready_events.swap(post_delivery_events_);
    }
    for (const PostDeliveryEvent &event : ready_events) {
        auto ctx = getConnectionContextById(event.conn_id);
        if (!ctx) {
            continue;
        }

        const bool submitted = event.finish_current_request ? finishCurrentRequestAndSubmitNext(ctx, event.conn_id)
                                                            : submitNextQueuedRequestIfIdle(ctx, event.conn_id);
        if (!submitted) {
            closeClientFd(ctx->fd(), "submit_request_task_failed");
        }
    }
}

bool TcpServer::createWorkerEventFd() {
    if (worker_event_fd_ != -1) {
        return true;
    }

    worker_event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (worker_event_fd_ == -1) {
        perror("eventfd create failed");
        return false;
    }
    return true;
}

bool TcpServer::registerWorkerEventFd() {
    if (epoll_fd_ == -1 || worker_event_fd_ == -1) {
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = worker_event_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, worker_event_fd_, &ev) < 0) {
        perror("epoll_ctl add worker_event_fd failed");
        return false;
    }
    return true;
}

void TcpServer::onWritable(int fd) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        std::cerr << "can't find connection context for fd=" << fd << std::endl;
        return;
    }

    while (true) {
        std::string data;
        if (!context->peekPendingSend(data)) {
            // No more pending data to send, disable writable event.
            if (!disableWritableEvent(fd)) {
                closeClientFd(fd, "disable_write_event_failed");
            }
            break;
        }

        ssize_t n = context->sendSome(data.c_str(), data.size());
        if (n == 0) {
            // send() returning 0 on stream sockets is treated as a closed peer path.
            closeClientFd(fd, "peer_closed_on_send");
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Socket buffer is full, keep pending bytes and wait for next EPOLLOUT.
                break;
            } else {
                closeClientFd(fd, "send_error");
                return;
            }
        }

        context->touchOnSend(static_cast<size_t>(n));
        context->consumePendingSend(static_cast<size_t>(n));
    }
}

bool TcpServer::appendPendingSend(int fd, const std::string &data) {
    auto context = getConnectionContextByFd(fd);
    if (context) {
        context->appendPendingSend(data);
        return true;
    }
    return false;
}

bool TcpServer::peekPendingChunkCopy(int fd, std::string &out) {
    auto context = getConnectionContextByFd(fd);
    if (context) {
        return context->peekPendingSend(out);
    }
    out.clear();
    return false;
}

bool TcpServer::hasPendingSend(int fd) {
    auto context = getConnectionContextByFd(fd);
    return context && context->hasPendingSend();
}

void TcpServer::consumePendingSend(int fd, size_t sent) {
    auto context = getConnectionContextByFd(fd);
    if (context) {
        context->consumePendingSend(sent);
    }
}

bool TcpServer::enableWritableEvent(int fd) {
    if (epoll_fd_ == -1) {
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLOUT;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl mod enable EPOLLOUT failed");
        return false;
    }
    return true;
}

bool TcpServer::disableWritableEvent(int fd) {
    if (epoll_fd_ == -1) {
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0) {
        perror("epoll_ctl mod disable EPOLLOUT failed");
        return false;
    }
    return true;
}

std::shared_ptr<ConnectionContext> TcpServer::getConnectionContextByFd(int fd) {
    std::lock_guard<std::mutex> lock(fd_to_context_mutex_);
    auto it = fd_to_context_.find(fd);
    if (it == fd_to_context_.end()) {
        return nullptr;
    }
    return it->second;
}

void TcpServer::logConnectionDisconnected(const ConnectionMeta &meta, const std::string &reason) {
    const auto now = std::chrono::steady_clock::now();
    const auto alive_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.last_active_at).count();
    std::cout << "[disconnected] conn_id=" << meta.conn_id << " reason=" << reason << " peer=" << meta.peer_ip << ":"
              << meta.peer_port << " recv_count=" << meta.recv_count << " send_count=" << meta.send_count
              << " recv_bytes=" << meta.recv_bytes << " send_bytes=" << meta.sent_bytes << " idle_ms=" << alive_ms
              << std::endl;
}

std::shared_ptr<ConnectionContext> TcpServer::getConnectionContextById(chat::ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        return nullptr;
    }
    return it->second;
}
