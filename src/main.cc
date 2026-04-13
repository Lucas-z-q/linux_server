#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include "EchoHandler.h"
#include "net/TcpServer.h"

/**
 * @file main.cc
 * @brief Entry point for starting the TCP echo server.
 */

/**
 * @brief Creates and runs the TCP server.
 * @return 0 on success, non-zero on startup failure.
 */
int main() {
    EchoHandler tmp;
    TcpServer server("127.0.0.1", 8080, tmp);
    if (!server.start()) {
        perror("server start failed");
        return 1;
    }
    return 0;
}
