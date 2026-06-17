#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

#include "fake_redis_client.h"
#include "net/IMessageHandler.h"
#include "net/TcpServer.h"
#include "server/redis_session_store.h"

namespace {

using namespace std::chrono_literals;

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

class TimeoutHandler : public IMessageHandler {
   public:
    explicit TimeoutHandler(chat::RedisSessionStore* session_store = nullptr) : session_store_(session_store) {}

    HandleResult handle(const std::string& request, const RequestContext& context) override {
        HandleResult result;
        if (request == "login") {
            result.response = "login_ok";
            result.session_action = SessionAction::BIND;
            std::string token(64, '0');
            const std::string suffix = std::to_string(context.conn_id);
            token.replace(token.size() - suffix.size(), suffix.size(), suffix);
            result.pending_session = {true, 10001, "alice", token};
            return result;
        }
        if (request == "logout") {
            result.response = "logout_ok";
            result.session_action = SessionAction::UNBIND;
            return result;
        }
        if (request == "slow") {
            slow_started.store(true);
            std::this_thread::sleep_for(slow_duration);
            result.response = "slow_ok";
            return result;
        }
        if (request == "large") {
            result.response.assign(16 * 1024 * 1024, 'x');
            large_handled.store(true);
            return result;
        }
        result.response = "ok";
        return result;
    }

    void onConnectionClosed(chat::ConnectionId conn_id) override {
        cleanup_started.fetch_add(1);
        if (cleanup_delay.count() > 0) {
            std::this_thread::sleep_for(cleanup_delay);
        }

        std::optional<chat::ConnectionSession> session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            const auto it = sessions_.find(conn_id);
            if (it != sessions_.end()) {
                session = it->second;
                sessions_.erase(it);
            }
        }
        if (session && session_store_ != nullptr) {
            session_store_->ClearPresence(conn_id, *session);
        }
        cleanup_finished.fetch_add(1);
    }

    void applyBindSession(chat::ConnectionId conn_id, const chat::ConnectionSession& session) override {
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            sessions_[conn_id] = session;
        }
        if (session_store_ != nullptr) {
            session_store_->Bind(conn_id, session, 1);
        }
    }

    void applyUnbindSession(chat::ConnectionId conn_id) override {
        std::optional<chat::ConnectionSession> session;
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            const auto it = sessions_.find(conn_id);
            if (it != sessions_.end()) {
                session = it->second;
                sessions_.erase(it);
            }
        }
        if (session && session_store_ != nullptr) {
            session_store_->ClearPresence(conn_id, *session);
            session_store_->RevokeToken(session->token);
        }
    }

    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        std::lock_guard<std::mutex> lock(session_mutex_);
        const auto it = sessions_.find(conn_id);
        return it != sessions_.end() && it->second.user_id == user_id;
    }

    std::atomic<int> cleanup_started{0};
    std::atomic<int> cleanup_finished{0};
    std::atomic<bool> slow_started{false};
    std::atomic<bool> large_handled{false};
    std::chrono::milliseconds slow_duration{300};
    std::chrono::milliseconds cleanup_delay{0};

   private:
    chat::RedisSessionStore* session_store_;
    std::mutex session_mutex_;
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions_;
};

class ServerHarness {
   public:
    ServerHarness(IMessageHandler& handler, TcpServerTimeoutOptions options)
        : server_("127.0.0.1", 0, handler, options) {}

    ~ServerHarness() { Stop(); }

    bool Start() {
        thread_ = std::thread([this]() { start_result_.store(server_.start()); });
        return WaitFor([this]() { return server_.getPort() != 0; }, 2s);
    }

    void Stop() {
        if (!thread_.joinable()) {
            return;
        }
        server_.stop();
        thread_.join();
    }

    uint16_t port() const { return server_.getPort(); }
    TcpServer& server() { return server_; }

   private:
    TcpServer server_;
    std::thread thread_;
    std::atomic<bool> start_result_{false};
};

int Connect(uint16_t port, int receive_buffer = 0) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    if (receive_buffer > 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    }

    struct sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool SendPacket(int fd, const std::string& packet) {
    const std::string encoded = packet + "\n";
    return send(fd, encoded.data(), encoded.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(encoded.size());
}

std::optional<std::string> ReceivePacket(int fd, std::chrono::milliseconds timeout) {
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd event {
            fd, POLLIN | POLLHUP | POLLERR, 0
        };
        const int remaining_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (poll(&event, 1, std::max(1, remaining_ms)) <= 0) {
            continue;
        }
        char buffer[4096];
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) {
            return std::nullopt;
        }
        received.append(buffer, static_cast<size_t>(count));
        const size_t newline = received.find('\n');
        if (newline != std::string::npos) {
            return received.substr(0, newline);
        }
    }
    return std::nullopt;
}

bool WaitForEof(int fd, std::chrono::milliseconds timeout) {
    return WaitFor(
        [fd]() {
            char buffer[4096];
            const ssize_t count = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
            if (count == 0) {
                return true;
            }
            if (count < 0) {
                return errno == ECONNRESET;
            }
            return false;
        },
        timeout);
}

TcpServerTimeoutOptions MakeOptions(uint32_t idle_ms, uint32_t heartbeat_ms) {
    TcpServerTimeoutOptions options;
    options.idle_timeout_ms = idle_ms;
    options.heartbeat_timeout_ms = heartbeat_ms;
    options.scan_interval_ms = 10;
    return options;
}

TEST(TcpServerTimeoutTest, IdleConnectionClosesBeforeAsyncCleanupFinishes) {
    TimeoutHandler handler;
    handler.cleanup_delay = 300ms;
    ServerHarness server(handler, MakeOptions(80, 1000));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(WaitForEof(fd, 2s));
    ASSERT_TRUE(WaitFor([&handler]() { return handler.cleanup_started.load() == 1; }, 1s));
    EXPECT_EQ(handler.cleanup_finished.load(), 0);
    EXPECT_TRUE(WaitFor([&handler]() { return handler.cleanup_finished.load() == 1; }, 1s));

    close(fd);
}

TEST(TcpServerTimeoutTest, ActiveConnectionIsRetained) {
    TimeoutHandler handler;
    ServerHarness server(handler, MakeOptions(100, 1000));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(SendPacket(fd, "ping"));
        const auto response = ReceivePacket(fd, 500ms);
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(*response, "ok");
        std::this_thread::sleep_for(50ms);
    }
    EXPECT_EQ(handler.cleanup_started.load(), 0);

    close(fd);
}

TEST(TcpServerTimeoutTest, AuthenticatedConnectionUsesHeartbeatTimeout) {
    TimeoutHandler handler;
    ServerHarness server(handler, MakeOptions(2000, 80));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendPacket(fd, "login"));
    ASSERT_EQ(ReceivePacket(fd, 500ms), std::optional<std::string>("login_ok"));
    EXPECT_TRUE(WaitForEof(fd, 2s));

    close(fd);
}

TEST(TcpServerTimeoutTest, RequestInProgressDefersTimeout) {
    TimeoutHandler handler;
    handler.slow_duration = 300ms;
    ServerHarness server(handler, MakeOptions(60, 1000));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendPacket(fd, "slow"));
    ASSERT_TRUE(WaitFor([&handler]() { return handler.slow_started.load(); }, 500ms));
    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(handler.cleanup_started.load(), 0);
    ASSERT_EQ(ReceivePacket(fd, 1s), std::optional<std::string>("slow_ok"));

    close(fd);
}

TEST(TcpServerTimeoutTest, PendingSendDefersTimeout) {
    TimeoutHandler handler;
    ServerHarness server(handler, MakeOptions(60, 1000));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port(), 4096);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendPacket(fd, "large"));
    ASSERT_TRUE(WaitFor([&handler]() { return handler.large_handled.load(); }, 500ms));
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(handler.cleanup_started.load(), 0);

    close(fd);
}

TEST(TcpServerTimeoutTest, MultipleDevicesKeepAuthenticationMetadata) {
    TimeoutHandler handler;
    ServerHarness server(handler, MakeOptions(1000, 80));
    ASSERT_TRUE(server.Start());

    const int old_fd = Connect(server.port());
    const int new_fd = Connect(server.port());
    ASSERT_GE(old_fd, 0);
    ASSERT_GE(new_fd, 0);
    ASSERT_TRUE(SendPacket(old_fd, "login"));
    ASSERT_EQ(ReceivePacket(old_fd, 500ms), std::optional<std::string>("login_ok"));
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(SendPacket(new_fd, "login"));
    ASSERT_EQ(ReceivePacket(new_fd, 500ms), std::optional<std::string>("login_ok"));

    std::this_thread::sleep_for(150ms);
    EXPECT_EQ(ReceivePacket(old_fd, 500ms), std::nullopt);
    EXPECT_EQ(ReceivePacket(new_fd, 500ms), std::nullopt);

    close(old_fd);
    close(new_fd);
}

TEST(TcpServerTimeoutTest, LogoutClearsAuthenticationMetadata) {
    TimeoutHandler handler;
    ServerHarness server(handler, MakeOptions(1000, 80));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendPacket(fd, "login"));
    ASSERT_EQ(ReceivePacket(fd, 500ms), std::optional<std::string>("login_ok"));
    ASSERT_TRUE(SendPacket(fd, "logout"));
    ASSERT_EQ(ReceivePacket(fd, 500ms), std::optional<std::string>("logout_ok"));

    std::this_thread::sleep_for(150ms);
    ASSERT_TRUE(SendPacket(fd, "ping"));
    ASSERT_EQ(ReceivePacket(fd, 500ms), std::optional<std::string>("ok"));

    close(fd);
}

TEST(TcpServerTimeoutTest, TimeoutCleanupDeletesRedisPresence) {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-timeout";
    chat::RedisSessionStore store(&redis, config);
    TimeoutHandler handler(&store);
    ServerHarness server(handler, MakeOptions(2000, 80));
    ASSERT_TRUE(server.Start());

    const int fd = Connect(server.port());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(SendPacket(fd, "login"));
    ASSERT_EQ(ReceivePacket(fd, 500ms), std::optional<std::string>("login_ok"));
    ASSERT_TRUE(WaitFor([&store]() { return store.GetPresence(10001).has_value(); }, 500ms));

    ASSERT_TRUE(WaitForEof(fd, 2s));
    EXPECT_TRUE(WaitFor([&store]() { return !store.GetPresence(10001).has_value(); }, 1s));

    close(fd);
}

}  // namespace
