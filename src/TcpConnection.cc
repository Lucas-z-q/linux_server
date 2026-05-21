#include "net/TcpConnection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <utility>

/**
 * @file TcpConnection.cc
 * @brief Implements TcpConnection socket operations.
 */

TcpConnection::TcpConnection(int conn_fd, const std::string& peer_ip, uint16_t peer_port)
    : conn_fd_(conn_fd), peer_ip_(peer_ip), peer_port_(peer_port) {}

TcpConnection::~TcpConnection() { close(); }

TcpConnection::TcpConnection(TcpConnection&& other) noexcept {
    conn_fd_ = other.conn_fd_;
    peer_ip_ = std::move(other.peer_ip_);
    peer_port_ = other.peer_port_;
    other.conn_fd_ = -1;
    other.peer_ip_.clear();
    other.peer_port_ = 0;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    close();  // 关闭当前的连接
    conn_fd_ = other.conn_fd_;
    peer_ip_ = std::move(other.peer_ip_);
    peer_port_ = other.peer_port_;
    // 移动后，将other的conn_fd_设置为-1，防止other析构时关闭fd
    // 并且将other的peer_ip_和peer_port_清空
    other.conn_fd_ = -1;
    other.peer_ip_.clear();
    other.peer_port_ = 0;
    return *this;
}

bool TcpConnection::isValid() const { return conn_fd_ != -1; }

int TcpConnection::fd() const { return conn_fd_; }

std::string TcpConnection::peerIp() const { return peer_ip_; }

uint16_t TcpConnection::peerPort() const { return peer_port_; }

void TcpConnection::close() {
    if (isValid()) {
        ::close(conn_fd_);
        conn_fd_ = -1;
    }
}

ssize_t TcpConnection::recv(char* buffer, size_t size) {
    if (!isValid()) {
        return -1;
    }
    return ::recv(conn_fd_, buffer, size, 0);
}

ssize_t TcpConnection::sendSome(const char* data, size_t len) {
    if (!isValid()) {
        return -1;
    }
    return ::send(conn_fd_, data, len, 0);
}

int TcpConnection::releaseFd() {
    int fd = conn_fd_;
    conn_fd_ = -1;
    return fd;
}

void TcpConnection::reset(int fd) {
    if (conn_fd_ == fd) {
        return;
    }

    close();
    conn_fd_ = fd;
}
