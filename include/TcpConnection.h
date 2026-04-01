#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class TcpConnection {
public:
    TcpConnection(int conn_fd, const std::string& peer_ip, uint16_t peer_port);
    ~TcpConnection();

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    ssize_t recv(char* buffer, size_t size);
    bool sendAll(const char* data, size_t len);
    void close();

    bool isValid() const;
    int fd() const;
    std::string peerIp() const;
    uint16_t peerPort() const;

private:
    int conn_fd_;
    std::string peer_ip_;
    uint16_t peer_prot_;
};