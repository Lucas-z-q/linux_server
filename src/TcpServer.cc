#include "TcpServer.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include "TcpConnection.h"

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
    socklen_t client_len = sizeof(client_addr);
    while (true) {
        int client_fd = accept(listen_fd_, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0) {
            perror("Accept failed");
            stop();
            return;
        }

        std::cout << "Client accept! "
                  << "IP: " << inet_ntoa(client_addr.sin_addr)
                  << ", port: " << ntohs(client_addr.sin_port) << std::endl;

        handleClient(client_fd, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    }
}

void TcpServer::handleClient(int conn_fd, const std::string& peer_id, uint16_t peer_port) {
    TcpConnection connect(conn_fd, peer_id, peer_port);

    char buffer[1024];
    while (true) {
        memset(buffer, 0, sizeof(buffer));
        ssize_t n = connect.recv(buffer, sizeof(buffer) - 1);
        if (n > 0) {
            std::string request(buffer);
            std::string ret = handler_.handle(request);
            connect.sendAll(&ret[0], ret.length());
            continue;
        }
        if (n == 0) {
            break;
        }
        if (n < 0) {
            perror("recive failed");
            break;
        }
    }
}