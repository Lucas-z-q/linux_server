#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

/**
 * @file server.cc
 * @brief Standalone legacy TCP server example (not built by current CMake target).
 */

/**
 * @brief Demonstration server entry point.
 * @return 0 on normal exit, non-zero on startup error.
 */
int main() {
    // 创建套接字
    //  AF_INET:IPV4,SOCK_STREAM:TCP
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_in server_addr;  // 准备一个IPV4地址结构
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 指定监听本机所有地址
    server_addr.sin_port = htons(8080);               // 监听特定端口8080

    // int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    int bind_ret = bind(server_fd, (struct sockaddr*)&server_addr,
                        sizeof(server_addr));  // 把这个地址绑定给server_fd
    if (bind_ret == -1) {
        perror("Bind failed");
        close(server_fd);
        return 1;
    }

    // int listen(int sockfd,int backlog);
    if (listen(server_fd, 5) == -1)  // 最多允许5个客户端连接
    {
        perror("Listen failed");
        close(server_fd);
        return 1;
    }

    // int accept(int sockfd, struct sockaddr*addr,socklen_t*addrlen);
    char buffer[1024];
    while (true) {
        struct sockaddr_in cliaddr;
        socklen_t cliaddr_len = sizeof(cliaddr);
        int conn_fd = accept(server_fd, (struct sockaddr*)&cliaddr, &cliaddr_len);
        if (conn_fd == -1) {
            perror("Accept failed");
            continue;  // 继续等待下一个连接
        }

        std::cout << "client connected\n";

        memset(buffer, 0, sizeof(buffer));
        ssize_t n = read(conn_fd, buffer, sizeof(buffer) - 1);
        std::cout << "client says: " << buffer << std::endl;

        const char* msg = "message received";
        write(conn_fd, msg, strlen(msg));

        close(conn_fd);
    }
    close(server_fd);
    return 0;
}
