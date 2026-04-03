#include "TcpServer.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include "TcpConnection.h"

/**
 * @file TcpServer.cc
 * @brief Implements TcpServer lifecycle and client handling loop.
 */

TcpServer::TcpServer(const std::string& ip, uint16_t port, IMessageHandler& handler)
    : listen_fd_(-1), ip_(ip), port_(port), handler_(handler) {}

TcpServer::~TcpServer() {
    stop();
}

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

    if (bind(listen_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
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
    if (listen_fd_ != -1) {
        close(listen_fd_);
    }
    listen_fd_ = -1;
}

bool TcpServer::start() {
    if (!createListenSocket())
        return false;

    if (!bindAddress())
        return false;

    if (!startListen())
        return false;

    acceptLoop();
    return true;
}

void TcpServer::acceptLoop() {
    struct sockaddr_in client_addr;
    while (true) {
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            perror("Accept failed");
            stop();
            return;
        }

        std::cout << "Client accept! "
                  << "IP: " << inet_ntoa(client_addr.sin_addr)
                  << ", port: " << ntohs(client_addr.sin_port) << std::endl;

        uint64_t conn_id = registerConnection(client_fd, inet_ntoa(client_addr.sin_addr),
                                              ntohs(client_addr.sin_port));
        handleClient(client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
                     conn_id);
    }
}

void TcpServer::handleClient(int conn_fd, const std::string& peer_id, uint16_t peer_port,
                             uint64_t conn_id) {
    TcpConnection connect(conn_fd, peer_id, peer_port);

    std::string close_reason = "peer_closed";
    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = connect.recv(buffer, sizeof(buffer) - 1);
        if (n > 0) {
            touchOnRecv(conn_id, static_cast<size_t>(n));

            std::string request(buffer, static_cast<size_t>(n));
            std::string ret = handler_.handle(request);
            if (!ret.empty() && connect.sendAll(ret.data(), ret.size())) {
                touchOnSend(conn_id, ret.size());
            }
            continue;
        }
        if (n == 0) {
            close_reason = "peer_closed";
            break;
        }
        if (n < 0) {
            perror("recive failed");
            close_reason = "recv_error";
            break;
        }
    }

    unregisterConnection(conn_id, close_reason);
}

uint64_t TcpServer::registerConnection(int conn_fd, const std::string& peer_ip,
                                       uint16_t peer_port) {
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

void TcpServer::touchOnRecv(uint64_t conn_id, size_t bytes) {
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end())
            return;
        ConnectionMeta* meta = &it->second;
        meta->last_active_at = std::chrono::steady_clock::now();
        meta->recv_count += 1;
        meta->recv_bytes += bytes;
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
        ConnectionMeta* meta = &it->second;
        meta->last_active_at = std::chrono::steady_clock::now();
        meta->send_count += 1;
        meta->sent_bytes += bytes;
    }
}

void TcpServer::unregisterConnection(uint64_t conn_id, const std::string& reason) {
    ConnectionMeta snapshot{};
    bool found = false;
    {
        // Remove once under lock to keep close path idempotent.
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(conn_id);
        if (it == connections_.end()) {
            return;
        }
        it->second.state = ConnectionMeta::State::CLOSING;
        snapshot = it->second;
        connections_.erase(it);
        found = true;
    }

    if (!found) {
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

void TcpServer::logConnectionMeta(const ConnectionMeta& meta) {
    const auto now = std::chrono::system_clock::to_time_t(meta.connected_at);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%F %T");

    std::cout << "[connected] conn_id=" << meta.conn_id << " peer=" << meta.peer_ip << ":"
              << meta.peer_port << " fd=" << meta.fd << " at=" << oss.str() << std::endl;
}
