#include "net/TcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/logger.h"

/**
 * @file TcpServer.cc
 * @brief Implements TcpServer lifecycle with epoll-based read/write event handling.
 */

TcpServer::TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler,
                     TcpServerTimeoutOptions timeout_options)
    : listen_fd_(-1),
      epoll_fd_(-1),
      worker_event_fd_(-1),
      timer_fd_(-1),
      ip_(ip),
      port_(port),
      handler_(handler),
      worker_pool_(kDefaultWorkerThreads),
      timeout_options_(timeout_options),
      stopping_(false) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::createListenSocket() {
    // 根据 ip_ 確定地址族，使 socket 类型与 bindAddress() 一致。
    struct in_addr addr4;
    struct in6_addr addr6;
    int af = AF_INET;  // 默认 IPv4
    if (inet_pton(AF_INET6, ip_.c_str(), &addr6) == 1) {
        af = AF_INET6;
    } else if (inet_pton(AF_INET, ip_.c_str(), &addr4) != 1) {
        // ip_ 应已由 ConfigLoader 校验，此分支理论上不会进入。
        LOG_ERROR("TcpServer") << "create listen socket failed: invalid IP address ip=" << ip_;
        return false;
    }
    listen_fd_ = socket(af, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        LOG_ERROR("TcpServer") << "socket failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }
    const int reuse_address = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) < 0) {
        LOG_ERROR("TcpServer") << "setsockopt SO_REUSEADDR failed errno=" << errno << " error=" << std::strerror(errno);
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

    struct sockaddr *bind_addr = nullptr;
    socklen_t bind_len = 0;

    if (inet_pton(AF_INET, ip_.c_str(), &addr4.sin_addr) == 1) {
        bind_addr = reinterpret_cast<struct sockaddr *>(&addr4);
        bind_len = sizeof(addr4);
    } else if (inet_pton(AF_INET6, ip_.c_str(), &addr6.sin6_addr) == 1) {
        bind_addr = reinterpret_cast<struct sockaddr *>(&addr6);
        bind_len = sizeof(addr6);
    } else {
        LOG_ERROR("TcpServer") << "bind failed: invalid IP address ip=" << ip_;
        return false;
    }

    if (bind(listen_fd_, bind_addr, bind_len) < 0) {
        LOG_ERROR("TcpServer") << "bind failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }

    if (requested_port == 0) {
        socklen_t len = bind_len;
        if (getsockname(listen_fd_, bind_addr, &len) == 0) {
            if (bind_addr->sa_family == AF_INET) {
                port_.store(ntohs(reinterpret_cast<struct sockaddr_in *>(bind_addr)->sin_port));
            } else {
                port_.store(ntohs(reinterpret_cast<struct sockaddr_in6 *>(bind_addr)->sin6_port));
            }
        }
    }
    return true;
}

bool TcpServer::startListen() {
    if (listen(listen_fd_, 5) < 0) {
        LOG_ERROR("TcpServer") << "listen failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }
    return true;
}

void TcpServer::stop() {
    bool expected = false;
    if (!stopping_.compare_exchange_strong(expected, true)) {
        return;
    }

    std::vector<RemotePushTask> pending_remote_pushes;
    {
        std::lock_guard<std::mutex> lock(remote_push_tasks_mutex_);
        pending_remote_pushes.swap(remote_push_tasks_);
    }
    for (RemotePushTask &task : pending_remote_pushes) {
        int pending = 0;
        if (task.state->compare_exchange_strong(pending, 2)) {
            try {
                task.completion->set_value(chat::RemoteDeliveryOutcome::kRetry);
            } catch (const std::future_error &) {
            }
        }
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

void TcpServer::setPreShutdownHook(std::function<void()> hook) { pre_shutdown_hook_ = std::move(hook); }

chat::RemoteDeliveryOutcome TcpServer::deliverRemotePush(const chat::RemotePushEvent &event,
                                                         std::chrono::milliseconds timeout) {
    if (stopping_.load() || event.target_connection_id == 0 || event.to_user_id <= 0 || event.payload.empty()) {
        return chat::RemoteDeliveryOutcome::kInvalidTarget;
    }

    auto completion = std::make_shared<std::promise<chat::RemoteDeliveryOutcome>>();
    auto state = std::make_shared<std::atomic<int>>(0);
    std::future<chat::RemoteDeliveryOutcome> result = completion->get_future();
    {
        std::lock_guard<std::mutex> event_fd_lock(worker_event_fd_mutex_);
        if (stopping_.load() || worker_event_fd_ == -1) {
            return chat::RemoteDeliveryOutcome::kRetry;
        }
        {
            std::lock_guard<std::mutex> task_lock(remote_push_tasks_mutex_);
            remote_push_tasks_.push_back({event, completion, state});
        }
        const uint64_t notify_value = 1;
        if (write(worker_event_fd_, &notify_value, sizeof(notify_value)) < 0 && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
            int pending = 0;
            if (state->compare_exchange_strong(pending, 2)) {
                return chat::RemoteDeliveryOutcome::kRetry;
            }
        }
    }

    if (result.wait_for(timeout) != std::future_status::ready) {
        int pending = 0;
        if (state->compare_exchange_strong(pending, 2)) {
            return chat::RemoteDeliveryOutcome::kRetry;
        }
        // I/O 线程已经认领任务，等待其完成可避免超时后重复投递正文。
        result.wait();
    }
    return result.get();
}

bool TcpServer::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        LOG_ERROR("TcpServer") << "fcntl F_GETFL failed fd=" << fd << " errno=" << errno
                               << " error=" << std::strerror(errno);
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_ERROR("TcpServer") << "fcntl F_SETFL failed fd=" << fd << " errno=" << errno
                               << " error=" << std::strerror(errno);
        return false;
    }
    return true;
}

bool TcpServer::start() {
    if (stopping_.load())
        return false;

    auto cleanup_resources = [this]() {
        stop();

        if (pre_shutdown_hook_) {
            pre_shutdown_hook_();
        }

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

        // 连接清理任务已全部提交，等待业务线程和 Redis 清理完成后再销毁通知句柄。
        worker_pool_.stop();

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
        if (timer_fd_ != -1) {
            close(timer_fd_);
            timer_fd_ = -1;
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
        LOG_ERROR("TcpServer") << "epoll_create1 failed errno=" << errno << " error=" << std::strerror(errno);
        cleanup_resources();
        return false;
    }

    if (!createWorkerEventFd() || !registerWorkerEventFd() || !createTimerFd() || !registerTimerFd()) {
        cleanup_resources();
        return false;
    }

    // Register the listen fd to receive incoming-connection notifications.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        LOG_ERROR("TcpServer") << "epoll_ctl add listen fd failed errno=" << errno << " error=" << std::strerror(errno);
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
    // 使用 sockaddr_storage 同时支持 IPv4 和 IPv6 客户端地址。
    struct sockaddr_storage client_addr;

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
            LOG_ERROR("TcpServer") << "epoll_wait failed errno=" << errno << " error=" << std::strerror(errno);
            break;
        }

        for (int i = 0; i < n; ++i) {
            const int fd = events[i].data.fd;
            const uint32_t event_mask = events[i].events;

            if (fd == worker_event_fd_) {
                onWorkerResultReadable();
                continue;
            }
            if (fd == timer_fd_) {
                onTimerReadable();
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
                int client_fd = accept(listen_fd_, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);
                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    LOG_ERROR("TcpServer") << "accept failed errno=" << errno << " error=" << std::strerror(errno);
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
                    LOG_ERROR("TcpServer") << "epoll_ctl add client fd failed fd=" << client_fd << " errno=" << errno
                                           << " error=" << std::strerror(errno);
                    close(client_fd);
                    continue;
                }

                registerConnection(
                    client_fd,
                    [&]() -> std::string {
                        char buf[INET6_ADDRSTRLEN] = {};
                        if (client_addr.ss_family == AF_INET6) {
                            const auto *a6 = reinterpret_cast<const struct sockaddr_in6 *>(&client_addr);
                            inet_ntop(AF_INET6, &a6->sin6_addr, buf, sizeof(buf));
                        } else {
                            const auto *a4 = reinterpret_cast<const struct sockaddr_in *>(&client_addr);
                            inet_ntop(AF_INET, &a4->sin_addr, buf, sizeof(buf));
                        }
                        return buf;
                    }(),
                    client_addr.ss_family == AF_INET6
                        ? ntohs(reinterpret_cast<const struct sockaddr_in6 *>(&client_addr)->sin6_port)
                        : ntohs(reinterpret_cast<const struct sockaddr_in *>(&client_addr)->sin_port));
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
    timeout_scan_queue_.push_back(conn_id);

    logConnectionMeta(context->meta());
    return context;
}

bool TcpServer::unregisterConnection(const std::shared_ptr<ConnectionContext> &context) {
    {
        std::scoped_lock lock(connections_mutex_, fd_to_context_mutex_);
        auto it = connections_.find(context->conn_id());
        if (it == connections_.end() || it->second != context) {
            return false;
        }
        context->markClosing();
        fd_to_context_.erase(context->fd());
        connections_.erase(it);
    }

    const chat::UserId user_id = context->meta().authenticated_user_id;
    const auto authenticated_it = authenticated_connections_.find(user_id);
    if (authenticated_it != authenticated_connections_.end()) {
        auto &connections = authenticated_it->second;
        connections.erase(std::remove(connections.begin(), connections.end(), context->conn_id()), connections.end());
        if (connections.empty()) {
            authenticated_connections_.erase(authenticated_it);
        }
    }
    return true;
}

void TcpServer::logConnectionMeta(const ConnectionMeta &meta) {
    LOG_INFO("TcpServer") << "connection established conn_id=" << meta.conn_id << " peer=" << meta.peer_ip << ":"
                          << meta.peer_port << " fd=" << meta.fd;
}

void TcpServer::closeClientFd(int fd, const std::string &reason) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        return;
    }
    const uint64_t conn_id = context->conn_id();

    // DEL may fail if the fd was already removed/closed by another path.
    if (epoll_fd_ != -1 && epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 && errno != ENOENT && errno != EBADF) {
        LOG_ERROR("TcpServer") << "epoll_ctl delete client fd failed fd=" << fd << " errno=" << errno
                               << " error=" << std::strerror(errno);
    }

    if (!unregisterConnection(context)) {
        return;
    }

    context->clearPendingSend();
    context->clearPendingRequests();
    context->closeConnection();
    context->markClosed();
    logConnectionDisconnected(context->meta(), reason);
    dispatchConnectionClosed(conn_id);
}

void TcpServer::dispatchConnectionClosed(chat::ConnectionId conn_id) {
    auto cleanup = [this, conn_id]() {
        try {
            handler_.onConnectionClosed(conn_id);
        } catch (const std::exception &) {
            LOG_ERROR("TcpServer") << "connection cleanup failed conn_id=" << conn_id << " exception=std_exception";
        } catch (...) {
            LOG_ERROR("TcpServer") << "connection cleanup failed conn_id=" << conn_id << " exception=unknown";
        }
    };

    try {
        worker_pool_.submit(cleanup);
    } catch (const std::exception &) {
        LOG_WARN("TcpServer") << "submit connection cleanup failed; running synchronously conn_id=" << conn_id;
        cleanup();
    }
}

void TcpServer::bindAuthenticatedConnection(const std::shared_ptr<ConnectionContext> &context, chat::UserId user_id) {
    unbindAuthenticatedConnection(context);

    auto &connections = authenticated_connections_[user_id];
    if (std::find(connections.begin(), connections.end(), context->conn_id()) == connections.end()) {
        connections.push_back(context->conn_id());
    }
    context->setAuthenticatedUserId(user_id);
}

void TcpServer::unbindAuthenticatedConnection(const std::shared_ptr<ConnectionContext> &context) {
    const chat::UserId user_id = context->meta().authenticated_user_id;
    const auto it = authenticated_connections_.find(user_id);
    if (it != authenticated_connections_.end()) {
        auto &connections = it->second;
        connections.erase(std::remove(connections.begin(), connections.end(), context->conn_id()), connections.end());
        if (connections.empty()) {
            authenticated_connections_.erase(it);
        }
    }
    context->clearAuthenticatedUserId();
}

void TcpServer::onReadable(int fd) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        LOG_ERROR("TcpServer") << "connection context not found fd=" << fd;
        return;
    }
    const uint64_t conn_id = context->conn_id();

    char buff[1024];
    while (true) {
        memset(buff, 0, sizeof(buff));
        ssize_t n = context->recv(buff, sizeof(buff) - 1);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                closeClientFd(fd, "recv_error");
                return;
            }
        }
        if (n == 0) {
            // Peer half-closed. Keep socket alive until pending response is drained.
            break;
        }

        context->touchOnRecv(static_cast<size_t>(n));
        LOG_DEBUG("TcpServer") << "message received conn_id=" << conn_id << " bytes=" << n;
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

            RequestTask task{std::weak_ptr<ConnectionContext>(context), conn_id, context->meta().peer_ip, packet};
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
                HandleResult result = handler_.handle(task.request, RequestContext{task.conn_id, task.peer_ip});

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
            } catch (const std::exception &) {
                LOG_ERROR("TcpServer") << "worker request failed exception=std_exception";
                auto context = task.context.lock();
                if (context) {
                    ResponseTask close_task{std::weak_ptr<ConnectionContext>(context), task.conn_id, "",
                                            next_response_task_id_.fetch_add(1), true};
                    if (!enqueueResponseTask(std::move(close_task))) {
                        context->clearPendingRequests();
                    }
                }
            } catch (...) {
                LOG_ERROR("TcpServer") << "worker request failed: unknown exception";
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
    } catch (const std::exception &) {
        LOG_ERROR("TcpServer") << "submit request task failed exception=std_exception";
        return false;
    }
    return true;
}

bool TcpServer::finishCurrentRequestAndSubmitNext(const std::shared_ptr<ConnectionContext> &context, uint64_t conn_id) {
    std::string next_request;
    if (!context->finishRequestAndPopNext(next_request)) {
        return true;
    }

    RequestTask next_task{std::weak_ptr<ConnectionContext>(context), conn_id, context->meta().peer_ip,
                          std::move(next_request)};
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

    RequestTask next_task{std::weak_ptr<ConnectionContext>(context), conn_id, context->meta().peer_ip,
                          std::move(next_request)};
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
            LOG_ERROR("TcpServer") << "write worker event fd failed errno=" << errno
                                   << " error=" << std::strerror(errno);
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
                    LOG_ERROR("TcpServer")
                        << "read worker event fd failed errno=" << errno << " error=" << std::strerror(errno);
                }
                break;
            }
        }
    }

    std::vector<RemotePushTask> remote_push_tasks;
    {
        std::lock_guard<std::mutex> lock(remote_push_tasks_mutex_);
        remote_push_tasks.swap(remote_push_tasks_);
    }
    for (RemotePushTask &task : remote_push_tasks) {
        int pending = 0;
        if (!task.state->compare_exchange_strong(pending, 1)) {
            continue;
        }
        chat::RemoteDeliveryOutcome outcome = chat::RemoteDeliveryOutcome::kInvalidTarget;
        if (handler_.isConnectionBoundToUser(task.event.target_connection_id, task.event.to_user_id)) {
            std::shared_ptr<ConnectionContext> target_context =
                getConnectionContextById(task.event.target_connection_id);
            if (target_context) {
                chat::PacketCodec codec;
                target_context->appendPendingSend(codec.encode(task.event.payload));
                if (enableWritableEvent(target_context->fd())) {
                    outcome = chat::RemoteDeliveryOutcome::kDelivered;
                } else {
                    closeClientFd(target_context->fd(), "enable_remote_push_write_failed");
                    outcome = chat::RemoteDeliveryOutcome::kRetry;
                }
            }
        }
        try {
            task.completion->set_value(outcome);
        } catch (const std::future_error &) {
            // 调用方超时不会取消任务，promise 仍可能正常完成。
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
                        } catch (const std::exception &) {
                            LOG_ERROR("TcpServer") << "mark delivered batch failed exception=std_exception";
                        } catch (...) {
                            LOG_ERROR("TcpServer") << "mark delivered batch failed: unknown exception";
                        }
                        enqueuePostDeliveryEvent(conn_id, true);
                    });
                    deferred_next_request = true;
                } catch (const std::exception &) {
                    LOG_ERROR("TcpServer") << "submit mark delivered batch failed exception=std_exception";
                } catch (...) {
                    LOG_ERROR("TcpServer") << "submit mark delivered batch failed: unknown exception";
                }
            }
        }

        if (task.session_action == SessionAction::BIND) {
            handler_.applyBindSession(task.conn_id, task.pending_session);
            bindAuthenticatedConnection(context, task.pending_session.user_id);
        } else if (task.session_action == SessionAction::UNBIND) {
            handler_.applyUnbindSession(task.conn_id);
            unbindAuthenticatedConnection(context);
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
                    } catch (const std::exception &) {
                        LOG_ERROR("TcpServer") << "mark delivered cross-connection push failed exception=std_exception";
                    } catch (...) {
                        LOG_ERROR("TcpServer") << "mark delivered cross-connection push failed: unknown exception";
                    }
                    auto ctx = getConnectionContextById(target_conn_id);
                    if (ctx) {
                        ctx->decrementPendingDeliveryMarks();
                        enqueuePostDeliveryEvent(target_conn_id, false);
                    }
                });
            } catch (const std::exception &) {
                target_ctx->decrementPendingDeliveryMarks();
                LOG_ERROR("TcpServer") << "submit mark delivered cross-connection push failed exception=std_exception";
                enqueuePostDeliveryEvent(target_conn_id, false);
            } catch (...) {
                target_ctx->decrementPendingDeliveryMarks();
                LOG_ERROR("TcpServer") << "submit mark delivered cross-connection push failed: unknown exception";
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
                    } catch (const std::exception &) {
                        LOG_ERROR("TcpServer") << "mark delivered same-connection push failed exception=std_exception";
                    } catch (...) {
                        LOG_ERROR("TcpServer") << "mark delivered same-connection push failed: unknown exception";
                    }
                    enqueuePostDeliveryEvent(conn_id, true);
                });
                deferred_next_request = true;
            } catch (const std::exception &) {
                LOG_ERROR("TcpServer") << "submit mark delivered same-connection push failed exception=std_exception";
            } catch (...) {
                LOG_ERROR("TcpServer") << "submit mark delivered same-connection push failed: unknown exception";
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
        LOG_ERROR("TcpServer") << "eventfd creation failed errno=" << errno << " error=" << std::strerror(errno);
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
        LOG_ERROR("TcpServer") << "epoll_ctl add worker event fd failed errno=" << errno
                               << " error=" << std::strerror(errno);
        return false;
    }
    return true;
}

bool TcpServer::createTimerFd() {
    if (timer_fd_ != -1) {
        return true;
    }

    timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd_ == -1) {
        LOG_ERROR("TcpServer") << "timerfd creation failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }

    const uint64_t interval_ms = std::max<uint32_t>(1, timeout_options_.scan_interval_ms);
    struct itimerspec timer_spec {};
    timer_spec.it_interval.tv_sec = static_cast<time_t>(interval_ms / 1000);
    timer_spec.it_interval.tv_nsec = static_cast<long>((interval_ms % 1000) * 1000000);
    timer_spec.it_value = timer_spec.it_interval;
    if (timerfd_settime(timer_fd_, 0, &timer_spec, nullptr) < 0) {
        LOG_ERROR("TcpServer") << "timerfd_settime failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }
    return true;
}

bool TcpServer::registerTimerFd() {
    if (epoll_fd_ == -1 || timer_fd_ == -1) {
        return false;
    }

    struct epoll_event ev {};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = timer_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, timer_fd_, &ev) < 0) {
        LOG_ERROR("TcpServer") << "epoll_ctl add timer fd failed errno=" << errno << " error=" << std::strerror(errno);
        return false;
    }
    return true;
}

void TcpServer::onTimerReadable() {
    uint64_t expirations = 0;
    while (read(timer_fd_, &expirations, sizeof(expirations)) < 0 && errno == EINTR) {
    }
    scanConnectionTimeouts();
}

void TcpServer::scanConnectionTimeouts() {
    const auto now = std::chrono::steady_clock::now();
    const size_t scan_count = std::min(kMaxTimeoutScanBatch, timeout_scan_queue_.size());
    for (size_t i = 0; i < scan_count; ++i) {
        const chat::ConnectionId conn_id = timeout_scan_queue_.front();
        timeout_scan_queue_.pop_front();

        auto context = getConnectionContextById(conn_id);
        if (!context) {
            continue;
        }

        bool timed_out = false;
        std::string reason;
        if (!context->shouldDeferTimeout()) {
            const ConnectionMeta &meta = context->meta();
            const auto last_io_at = std::max(meta.last_recv_at, meta.last_send_at);
            const auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_io_at);
            if (idle_ms >= std::chrono::milliseconds(timeout_options_.idle_timeout_ms)) {
                timed_out = true;
                reason = "idle_timeout";
            } else if (meta.authenticated_user_id > 0) {
                const auto heartbeat_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.last_active_at);
                if (heartbeat_ms >= std::chrono::milliseconds(timeout_options_.heartbeat_timeout_ms)) {
                    timed_out = true;
                    reason = "heartbeat_timeout";
                }
            }
        }

        if (timed_out) {
            closeClientFd(context->fd(), reason);
        } else if (getConnectionContextById(conn_id)) {
            timeout_scan_queue_.push_back(conn_id);
        }
    }
}

void TcpServer::onWritable(int fd) {
    auto context = getConnectionContextByFd(fd);
    if (!context) {
        LOG_ERROR("TcpServer") << "connection context not found for writable fd=" << fd;
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
        LOG_ERROR("TcpServer") << "epoll_ctl enable writable failed fd=" << fd << " errno=" << errno
                               << " error=" << std::strerror(errno);
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
        LOG_ERROR("TcpServer") << "epoll_ctl disable writable failed fd=" << fd << " errno=" << errno
                               << " error=" << std::strerror(errno);
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
    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.connected_at).count();
    const auto recv_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.last_recv_at).count();
    const auto send_idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - meta.last_send_at).count();
    LOG_INFO("TcpServer") << "connection closed conn_id=" << meta.conn_id << " user_id=" << meta.authenticated_user_id
                          << " reason=" << reason << " peer=" << meta.peer_ip << ":" << meta.peer_port
                          << " recv_count=" << meta.recv_count << " send_count=" << meta.send_count
                          << " recv_bytes=" << meta.recv_bytes << " send_bytes=" << meta.sent_bytes
                          << " duration_ms=" << duration_ms << " recv_idle_ms=" << recv_idle_ms
                          << " send_idle_ms=" << send_idle_ms;
}

std::shared_ptr<ConnectionContext> TcpServer::getConnectionContextById(chat::ConnectionId conn_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(conn_id);
    if (it == connections_.end()) {
        return nullptr;
    }
    return it->second;
}
