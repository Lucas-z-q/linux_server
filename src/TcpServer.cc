#include "net/TcpServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
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
      ip_(ip),
      port_(port),
      handler_(handler),
      worker_pool_(kDefaultWorkerThreads) {}

TcpServer::~TcpServer() { stop(); }

bool TcpServer::createListenSocket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1) {
        return false;
    }
    return true;
}

bool TcpServer::bindAddress() {
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(listen_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed.");
        stop();
        return false;
    }
    return true;
}

bool TcpServer::startListen() {
    if (listen(listen_fd_, 5) < 0) {
        perror("Listen failed.");
        stop();
        return false;
    }
    return true;
}

void TcpServer::stop() {
    // Snapshot first, then close each client via the unified close path.
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

    // Close epoll after clients are removed from it.
    if (epoll_fd_ != -1) {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (listen_fd_ != -1) {
        close(listen_fd_);
        listen_fd_ = -1;
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
    if (!createListenSocket())
        return false;

    if (!bindAddress())
        return false;

    if (!startListen())
        return false;

    // EPOLLET requires non-blocking sockets, otherwise edge-trigger behavior is unsafe.
    if (!set_nonblocking(listen_fd_)) {
        stop();
        return false;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        perror("epoll_create failed.");
        stop();
        return false;
    }

    // Register the listen fd to receive incoming-connection notifications.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0) {
        perror("epoll_ctl add listen_fd failed");
        stop();
        return false;
    }

    acceptLoop(epoll_fd_);
    return true;
}

void TcpServer::acceptLoop(int epoll_fd) {
    constexpr int kMaxEvents = 1024;
    struct epoll_event events[kMaxEvents];
    struct sockaddr_in client_addr;

    while (true) {
        int n = epoll_wait(epoll_fd, events, kMaxEvents, -1);
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
    auto context = std::make_shared<ConnectionContext>(conn_fd, conn_id, peer_ip, peer_port);

    {
        std::scoped_lock lock(connections_mutex_, fd_to_context_mutex_);
        connections_[conn_id] = context;
        fd_to_context_[conn_fd] = context;
    }

    logConnectionMeta(context->meta());
    return context;
}

void TcpServer::touchOnRecv(uint64_t conn_id, size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
            return;
        it->second->touchOnRecv(bytes);
    }

    // Lightweight per-message receive log.
    std::cout << "[message_received] conn_id=" << conn_id << " bytes=" << bytes << std::endl;
}

void TcpServer::touchOnSend(uint64_t conn_id, size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
            return;
        it->second->touchOnSend(bytes);
    }
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

    if (close(fd) < 0 && errno != EBADF) {
        perror("close client_fd failed");
    }

    context->clearPendingSend();
    handler_.onConnectionClosed(conn_id);
    unregisterConnection(conn_id, reason);
}

void TcpServer::onReadable(int fd) {
    uint64_t conn_id = 0;
    if (!getConnIdByFd(fd, conn_id)) {
        std::cerr << "can't find conn_id for fd=" << fd << std::endl;
        return;
    }

    char buff[1024];
    while (true) {
        memset(buff, 0, sizeof(buff));
        ssize_t n = recv(fd, buff, sizeof(buff) - 1, 0);
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

        touchOnRecv(conn_id, static_cast<size_t>(n));
        std::string chunk(buff, static_cast<size_t>(n));

        auto context = getConnectionContextByFd(fd);
        if (!context) {
            closeClientFd(fd, "missing_connection_context");
            return;
        }

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
            if (!submitRequestTask(std::move(task))) {
                closeClientFd(fd, "submit_request_task_failed");
                return;
            }
        }
    }
}

bool TcpServer::submitRequestTask(RequestTask task) {
    try {
        worker_pool_.submit([this, task = std::move(task)]() mutable {
            auto context = task.context.lock();
            if (!context) {
                return;
            }

            std::string response = handler_.handle(task.request, task.conn_id);
            (void)response;
        });
    } catch (const std::exception &ex) {
        std::cerr << "submit request task failed: " << ex.what() << std::endl;
        return false;
    }
    return true;
}

bool TcpServer::getConnIdByFd(int fd, uint64_t &conn_id) {
    auto context = getConnectionContextByFd(fd);
    if (context) {
        conn_id = context->conn_id();
        return true;
    }
    return false;
}

void TcpServer::onWritable(int fd) {
    uint64_t conn_id = 0;
    if (!getConnIdByFd(fd, conn_id)) {
        std::cerr << "can't find conn_id for fd=" << fd << std::endl;
        return;
    }

    while (true) {
        std::string data;
        if (!peekPendingChunkCopy(fd, data)) {
            // No more pending data to send, disable writable event.
            if (!disableWritableEvent(fd)) {
                closeClientFd(fd, "disable_write_event_failed");
            }
            break;
        }

        ssize_t n = send(fd, data.c_str(), data.size(), 0);
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

        touchOnSend(conn_id, static_cast<size_t>(n));
        consumePendingSend(fd, static_cast<size_t>(n));
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
