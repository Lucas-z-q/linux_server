#pragma once

#include <cstdint>
#include <string>
#include "IMessageHandler.h"

class TcpServer {
public:
    TcpServer(const std::string& ip, uint16_t port, IMessageHandler& handler);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start();
    void stop();

private:
    bool createListenSocket();
    bool bindAddress();
    bool startListen();
    void acceptLoop();
    void handleClient(int conn_fd, const std::string& peer_id, uint16_t peer_port);

private:
    int listen_fd_;
    std::string ip_;
    uint16_t port_;
    IMessageHandler& handler_;
};