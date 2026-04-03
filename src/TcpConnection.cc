#include "../include/TcpConnection.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

/**
 * @file TcpConnection.cc
 * @brief Implements TcpConnection socket operations.
 */

TcpConnection::TcpConnection(int conn_fd, const std::string& peer_ip, uint16_t peer_port)
    : conn_fd_(conn_fd), peer_ip_(peer_ip), peer_prot_(peer_port) {}

TcpConnection::~TcpConnection() {
    close();
}

bool TcpConnection::isValid() const {
    return conn_fd_ != -1;
}

int TcpConnection::fd() const {
    return conn_fd_;
}

std::string TcpConnection::peerIp() const {
    return peer_ip_;
}

uint16_t TcpConnection::peerPort() const {
    return peer_prot_;
}

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

bool TcpConnection::sendAll(const char* data, size_t len) {
    if (!isValid())
        return false;

    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t sent = ::send(conn_fd_, data + total_sent, len - total_sent, 0);
        if (sent <= 0) {
            return false;
        }
        total_sent += sent;
    }
    return true;
}
