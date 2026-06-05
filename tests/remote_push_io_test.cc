#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#include "net/IMessageHandler.h"
#include "net/TcpServer.h"

namespace {

class RemotePushHandler : public IMessageHandler {
   public:
    std::atomic<bool> bound{true};

    HandleResult handle(const std::string &request, const RequestContext &context) override {
        (void)request;
        (void)context;
        return {};
    }

    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        return bound.load() && conn_id == 1 && user_id == 2;
    }
};

uint16_t WaitForPort(TcpServer *server) {
    for (int i = 0; i < 100; ++i) {
        const uint16_t port = server->getPort();
        if (port != 0) {
            return port;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return 0;
}

int Connect(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    assert(connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
    return fd;
}

}  // namespace

int main() {
    RemotePushHandler handler;
    TcpServer server("127.0.0.1", 0, handler);
    std::thread server_thread([&]() { assert(server.start()); });

    const uint16_t port = WaitForPort(&server);
    assert(port != 0);
    const int client_fd = Connect(port);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    chat::RemotePushEvent event{"event-1", "message-1", 1,
                                2,         1,           R"({"msg_type":"message_push","data":{"content":"hello"}})"};
    const auto delivered = server.deliverRemotePush(event, std::chrono::milliseconds(500));
    assert(delivered == chat::RemoteDeliveryOutcome::kDelivered);

    char buffer[256] = {};
    const ssize_t received = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    assert(received > 0);
    assert(std::string(buffer, static_cast<std::size_t>(received)).find("message_push") != std::string::npos);

    handler.bound = false;
    const auto rebound = server.deliverRemotePush(event, std::chrono::milliseconds(500));
    assert(rebound == chat::RemoteDeliveryOutcome::kInvalidTarget);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(recv(client_fd, buffer, sizeof(buffer), MSG_DONTWAIT) == -1);

    close(client_fd);
    server.stop();
    server_thread.join();
    std::cout << "[PASS] remote push I/O test passed\n";
    return 0;
}
