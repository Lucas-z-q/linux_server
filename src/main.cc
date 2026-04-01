#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include "EchoHandler.h"
#include "TcpServer.h"

int main() {
    EchoHandler tmp;
    TcpServer server("127.0.0.1", 8080, tmp);
    if (!server.start()) {
        perror("server start failed");
        return 1;
    }
    return 0;
}