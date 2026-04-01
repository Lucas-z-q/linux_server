#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <iostream>

int main() {
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
    write(client_fd, buffer, strlen(buffer));

    char recv_buf[1024];
    memset(recv_buf, 0, sizeof(recv_buf));
    ssize_t n = read(client_fd, recv_buf, sizeof(recv_buf) - 1);
    if (n == -1) {
        perror("Read failed");
        close(client_fd);
        return 1;
    }

    std::cout << "server says: " << recv_buf << std::endl;

    close(client_fd);
    return 0;
}
