#include "net/TcpServer.h"
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

/**
 * @file TcpServer.cc
 * @brief Implements TcpServer lifecycle with epoll-based read/write event handling.
 */

TcpServer::TcpServer(const std::string &ip, uint16_t port, IMessageHandler &handler)
    : listen_fd_(-1), epoll_fd_(-1), ip_(ip), port_(port), handler_(handler) {}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::createListenSocket()
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ == -1)
    {
        return false;
    }
    return true;
}

bool TcpServer::bindAddress()
{
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(listen_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Bind failed.");
        stop();
        return false;
    }
    return true;
}

bool TcpServer::startListen()
{
    if (listen(listen_fd_, 5) < 0)
    {
        perror("Listen failed.");
        stop();
        return false;
    }
    return true;
}

void TcpServer::stop()
{
    // Snapshot first, then close each client via the unified close path.
    std::vector<int> client_fds;
    {
        std::lock_guard<std::mutex> lock(fd_to_conn_id_mutex);
        client_fds.reserve(fd_to_conn_id.size());
        for (const auto &entry : fd_to_conn_id)
        {
            client_fds.push_back(entry.first);
        }
    }
    for (int fd : client_fds)
    {
        closeClientFd(fd, "server_stop");
    }

    // Close epoll after clients are removed from it.
    if (epoll_fd_ != -1)
    {
        close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (listen_fd_ != -1)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool TcpServer::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl(F_GETFL) failed");
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl(F_SETFL) failed");
        return false;
    }
    return true;
}

bool TcpServer::start()
{
    if (!createListenSocket())
        return false;

    if (!bindAddress())
        return false;

    if (!startListen())
        return false;

    // EPOLLET requires non-blocking sockets, otherwise edge-trigger behavior is unsafe.
    if (!set_nonblocking(listen_fd_))
    {
        stop();
        return false;
    }

    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0)
    {
        perror("epoll_create failed.");
        stop();
        return false;
    }

    // Register the listen fd to receive incoming-connection notifications.
    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &event) < 0)
    {
        perror("epoll_ctl add listen_fd failed");
        stop();
        return false;
    }

    acceptLoop(epoll_fd_);
    return true;
}

void TcpServer::acceptLoop(int epoll_fd)
{
    constexpr int kMaxEvents = 1024;
    struct epoll_event events[kMaxEvents];
    struct sockaddr_in client_addr;

    while (true)
    {
        int n = epoll_wait(epoll_fd, events, kMaxEvents, -1);
        if (n < 0)
        {
            // Signal interruption is transient.
            if (errno == EINTR)
            {
                continue;
            }
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i)
        {
            const int fd = events[i].data.fd;
            const uint32_t event_mask = events[i].events;

            // For connected sockets, handle close/error/read/write events.
            if (fd != listen_fd_)
            {
                // Hard errors are handled first.
                if (event_mask & EPOLLERR)
                {
                    closeClientFd(fd, "peer_error");
                    continue;
                }

                // Read/write first, so half-close peers can still receive queued responses.
                if (event_mask & EPOLLIN)
                {
                    onReadable(fd);
                }
                if (event_mask & EPOLLOUT)
                {
                    onWritable(fd);
                }
                // After I/O handling, close only when peer is closed and no data remains to send.
                if (event_mask & (EPOLLHUP | EPOLLRDHUP))
                {
                    if (!hasPendingSend(fd))
                    {
                        closeClientFd(fd, "peer_hup_or_error");
                    }
                }
                continue;
            }

            // listen_fd_ is readable: accept all pending connections.
            while (true)
            {
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd_, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0)
                {
                    if (errno == EAGAIN || errno == EWOULDBLOCK)
                    {
                        break;
                    }
                    perror("Accept failed");
                    stop();
                    return;
                }

                if (!set_nonblocking(client_fd))
                {
                    close(client_fd);
                    continue;
                }

                // New connections start in read-only interest; EPOLLOUT is enabled on demand.
                struct epoll_event ev;
                ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
                ev.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
                {
                    perror("epoll_ctl add client_fd failed");
                    close(client_fd);
                    continue;
                }
                uint64_t conn_id =
                    registerConnection(client_fd, inet_ntoa(client_addr.sin_addr),
                                       ntohs(client_addr.sin_port));
                {
                    std::lock_guard<std::mutex> lock(fd_to_conn_id_mutex);
                    fd_to_conn_id[client_fd] = conn_id;
                }
                {
                    std::lock_guard<std::mutex> lock(packet_codecs_mutex);
                    packet_codecs_.emplace(client_fd, chat::PacketCodec());
                }
                std::cout
                    << "Client accept! "
                    << "IP: " << inet_ntoa(client_addr.sin_addr)
                    << ", port: " << ntohs(client_addr.sin_port) << std::endl;
            }
        }
    }
}

uint64_t TcpServer::registerConnection(int conn_fd, const std::string &peer_ip,
                                       uint16_t peer_port)
{
    // Generate a monotonic server-side connection id.
    uint64_t conn_id = next_conn_id_.fetch_add(1);

    // Initialize metadata snapshot for this new connection.
    ConnectionMeta meta{.conn_id = conn_id,
                        .fd = conn_fd,
                        .peer_ip = peer_ip,
                        .peer_port = peer_port,
                        .connected_at = std::chrono::system_clock::now(),
                        .last_active_at = std::chrono::steady_clock::now(),
                        .recv_count = 0,
                        .send_count = 0,
                        .recv_bytes = 0,
                        .sent_bytes = 0,
                        .state = ConnectionMeta::State::CONNECTED};
    {
        // Keep lock scope tight: only map mutation is protected.
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[conn_id] = meta;
    }

    // Log outside lock to avoid unnecessary lock hold during I/O.
    logConnectionMeta(meta);
    return conn_id;
}

void TcpServer::touchOnRecv(uint64_t conn_id, size_t bytes)
{
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
            return;
        ConnectionMeta *meta = &it->second;
        meta->last_active_at = std::chrono::steady_clock::now();
        meta->recv_count += 1;
        meta->recv_bytes += bytes;
    }

    // Lightweight per-message receive log.
    std::cout << "[message_received] conn_id=" << conn_id << " bytes=" << bytes << std::endl;
}

void TcpServer::touchOnSend(uint64_t conn_id, size_t bytes)
{
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
            return;
        ConnectionMeta *meta = &it->second;
        meta->last_active_at = std::chrono::steady_clock::now();
        meta->send_count += 1;
        meta->sent_bytes += bytes;
    }
}

void TcpServer::unregisterConnection(uint64_t conn_id, const std::string &reason)
{
    ConnectionMeta snapshot{};
    bool found = false;
    {
        // Remove once under lock to keep close path idempotent.
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
        {
            return;
        }
        it->second.state = ConnectionMeta::State::CLOSING;
        snapshot = it->second;
        connections_.erase(it);
        found = true;
    }

    if (!found)
    {
        return;
    }

    snapshot.state = ConnectionMeta::State::CLOSED;

    const auto now = std::chrono::steady_clock::now();
    const auto alive_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - snapshot.last_active_at)
            .count();
    std::cout << "[disconnected] conn_id=" << snapshot.conn_id << " reason=" << reason
              << " peer=" << snapshot.peer_ip << ":" << snapshot.peer_port
              << " recv_count=" << snapshot.recv_count << " send_count=" << snapshot.send_count
              << " recv_bytes=" << snapshot.recv_bytes << " send_bytes=" << snapshot.sent_bytes
              << " idle_ms=" << alive_ms << std::endl;
}

void TcpServer::logConnectionMeta(const ConnectionMeta &meta)
{
    const auto now = std::chrono::system_clock::to_time_t(meta.connected_at);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%F %T");

    std::cout << "[connected] conn_id=" << meta.conn_id << " peer=" << meta.peer_ip << ":"
              << meta.peer_port << " fd=" << meta.fd << " at=" << oss.str() << std::endl;
}

void TcpServer::closeClientFd(int fd, const std::string &reason)
{
    uint64_t conn_id = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(fd_to_conn_id_mutex);
        auto it = fd_to_conn_id.find(fd);
        if (it != fd_to_conn_id.end())
        {
            conn_id = it->second;
            fd_to_conn_id.erase(it);
            found = true;
        }
    }

    // DEL may fail if the fd was already removed/closed by another path.
    if (epoll_fd_ != -1 && epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) < 0 &&
        errno != ENOENT && errno != EBADF)
    {
        perror("epoll_ctl del client_fd failed");
    }

    if (close(fd) < 0 && errno != EBADF)
    {
        perror("close client_fd failed");
    }

    // Drop any queued outbound bytes for this fd to avoid stale buffer growth.
    {
        std::lock_guard<std::mutex> lock(pending_send_mutex);
        pending_send_.erase(fd);
    }
    // Drop packet codec state for this fd to avoid stale buffer growth.
    {
        std::lock_guard<std::mutex> lock(packet_codecs_mutex);
        packet_codecs_.erase(fd);
    }

    if (found)
    {
        handler_.onConnectionClosed(conn_id);
        unregisterConnection(conn_id, reason);
    }
}

void TcpServer::onReadable(int fd)
{
    uint64_t conn_id = 0;
    if (!getConnIdByFd(fd, conn_id))
    {
        std::cerr << "can't find conn_id for fd=" << fd << std::endl;
        return;
    }

    char buff[1024];
    while (true)
    {
        memset(buff, 0, sizeof(buff));
        ssize_t n = recv(fd, buff, sizeof(buff) - 1, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else
            {
                closeClientFd(fd, "recv_error");
                return;
            }
        }
        if (n == 0)
        {
            // Peer half-closed. Keep socket alive until pending response is drained.
            break;
        }

        touchOnRecv(conn_id, static_cast<size_t>(n));
        std::string chunk(buff, static_cast<size_t>(n));

        std::vector<std::string> packets;
        bool missing_packet_codec = false;
        bool invalid_packet = false;
        {
            std::lock_guard<std::mutex> lock(packet_codecs_mutex);
            auto it = packet_codecs_.find(fd);
            if (it == packet_codecs_.end())
            {
                missing_packet_codec = true;
            }
            else
            {
                invalid_packet = !it->second.feed(chunk, packets);
            }
        }

        if (missing_packet_codec)
        {
            closeClientFd(fd, "missing_packet_codec");
            return;
        }
        if (invalid_packet)
        {
            closeClientFd(fd, "packet_too_large");
            return;
        }

        bool queued_response = false;

        for (const std::string &packet : packets)
        {
            if (packet.empty())
            {
                continue;
            }

            std::string response = handler_.handle(packet, conn_id);
            if (response.empty())
            {
                continue;
            }

            chat::PacketCodec codec;
            appendPendingSend(fd, codec.encode(response));
            queued_response = true;
        }

        if (queued_response)
        {
            if (!enableWritableEvent(fd))
            {
                closeClientFd(fd, "enable_write_event_failed");
                return;
            }
        }
    }
}

bool TcpServer::getConnIdByFd(int fd, uint64_t &conn_id)
{
    std::lock_guard<std::mutex> lock(fd_to_conn_id_mutex);
    auto it = fd_to_conn_id.find(fd);
    if (it != fd_to_conn_id.end())
    {
        conn_id = it->second;
        return true;
    }
    return false;
}

void TcpServer::onWritable(int fd)
{
    uint64_t conn_id = 0;
    if (!getConnIdByFd(fd, conn_id))
    {
        std::cerr << "can't find conn_id for fd=" << fd << std::endl;
        return;
    }

    while (true)
    {
        std::string data;
        if (!peekPendingChunkCopy(fd, data))
        {
            // No more pending data to send, disable writable event.
            if (!disableWritableEvent(fd))
            {
                closeClientFd(fd, "disable_write_event_failed");
            }
            break;
        }

        ssize_t n = send(fd, data.c_str(), data.size(), 0);
        if (n == 0)
        {
            // send() returning 0 on stream sockets is treated as a closed peer path.
            closeClientFd(fd, "peer_closed_on_send");
            return;
        }
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Socket buffer is full, keep pending bytes and wait for next EPOLLOUT.
                break;
            }
            else
            {
                closeClientFd(fd, "send_error");
                return;
            }
        }

        touchOnSend(conn_id, static_cast<size_t>(n));
        consumePendingSend(fd, static_cast<size_t>(n));
    }
}

bool TcpServer::appendPendingSend(int fd, const std::string &data)
{
    std::lock_guard<std::mutex> lock(pending_send_mutex);
    pending_send_[fd] += data;
    return true;
}

bool TcpServer::peekPendingChunkCopy(int fd, std::string &out)
{
    std::lock_guard<std::mutex> lock(pending_send_mutex);
    auto it = pending_send_.find(fd);
    if (it == pending_send_.end() || it->second.empty())
    {
        out.clear();
        return false;
    }
    out = it->second;
    return true;
}

bool TcpServer::hasPendingSend(int fd)
{
    std::lock_guard<std::mutex> lock(pending_send_mutex);
    auto it = pending_send_.find(fd);
    return it != pending_send_.end() && !it->second.empty();
}

void TcpServer::consumePendingSend(int fd, size_t sent)
{
    if (sent == 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(pending_send_mutex);
    auto it = pending_send_.find(fd);
    if (it == pending_send_.end())
    {
        return;
    }

    if (sent >= it->second.size())
    {
        pending_send_.erase(it);
        return;
    }

    it->second.erase(0, sent);
}

bool TcpServer::enableWritableEvent(int fd)
{
    if (epoll_fd_ == -1)
    {
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP | EPOLLOUT;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        perror("epoll_ctl mod enable EPOLLOUT failed");
        return false;
    }
    return true;
}

bool TcpServer::disableWritableEvent(int fd)
{
    if (epoll_fd_ == -1)
    {
        return false;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        perror("epoll_ctl mod disable EPOLLOUT failed");
        return false;
    }
    return true;
}
