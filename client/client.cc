#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <string>

#include "codec/packet_codec.h"

/**
 * @file client.cc
 * @brief Simple interactive TCP client for manual server testing.
 */

/**
 * @brief Connects to the server, sends one line, and prints one response.
 * @return 0 on success, non-zero on failure.
 */
int main() {
    chat::PacketCodec codec;

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd == -1) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(8080);

    if (inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client_fd);
        return 1;
    }

    if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("Connect failed");
        close(client_fd);
        return 1;
    }

    char buffer[1024];
    memset(buffer, 0, sizeof(buffer));
    std::cin.getline(buffer, sizeof(buffer));  // 从标准输入读一行
    while (strlen(buffer) == 0) {
        std::cout << "没有正确输入内容，请重新输入\n";
        std::cin.getline(buffer, sizeof(buffer));
    }
    const std::string request = codec.encode(buffer);
    size_t total_sent = 0;
    while (total_sent < request.size()) {
        const ssize_t n = write(client_fd, request.data() + total_sent,
                                request.size() - total_sent);
        if (n <= 0) {
            perror("Write failed");
            close(client_fd);
            return 1;
        }
        total_sent += static_cast<size_t>(n);
    }

    std::string response;
    char recv_buf[1024];
    while (response.find('\n') == std::string::npos) {
        memset(recv_buf, 0, sizeof(recv_buf));
        const ssize_t n = read(client_fd, recv_buf, sizeof(recv_buf) - 1);
        if (n == -1) {
            perror("Read failed");
            close(client_fd);
            return 1;
        }
        if (n == 0) {
            std::cerr << "server closed connection before sending full response\n";
            close(client_fd);
            return 1;
        }
        response.append(recv_buf, static_cast<size_t>(n));
    }

    const std::size_t line_end = response.find('\n');
    std::cout << "server says: " << response.substr(0, line_end) << std::endl;

    close(client_fd);
    return 0;
}
