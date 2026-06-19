#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "model/connection_session.h"
#include "net/IMessageHandler.h"
#include "net/TcpServer.h"

// 一个模拟工作线程被慢速业务阻塞的 Handler
class SlowHandler : public IMessageHandler {
   public:
    HandleResult handle(const std::string& request, const RequestContext& context) override {
        (void)context;
        // 刻意让 worker 线程沉睡，以便触发在处理期间调用 server.stop()
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        HandleResult result;
        result.response = "dummy_response";
        return result;
    }
    void onConnectionClosed(uint64_t conn_id) override {}
    void applyBindSession(uint64_t conn_id, const chat::ConnectionSession& session) override {}
    void applyUnbindSession(uint64_t conn_id) override {}
    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        (void)conn_id;
        (void)user_id;
        return true;
    }
};

// 验证：重复调用 TcpServer::stop() 不崩溃，严格幂等
TEST(TcpServerShutdownTest, MultipleStopCallsAreSafe) {
    SlowHandler handler;
    TcpServer server("127.0.0.1", 0, handler);

    server.stop();

    EXPECT_NO_THROW({
        server.stop();
        server.stop();
    });
}

// 验证：start() 中间失败时，内部安全调用 stop() 退出
TEST(TcpServerShutdownTest, StartFailureDoesNotCrash) {
    // 1. 刻意占用一个端口，制造 Bind 冲突
    int dummy_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(dummy_fd, 0);
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    ASSERT_EQ(bind(dummy_fd, (struct sockaddr*)&addr, sizeof(addr)), 0);

    socklen_t len = sizeof(addr);
    ASSERT_EQ(getsockname(dummy_fd, (struct sockaddr*)&addr, &len), 0);
    uint16_t occupied_port = ntohs(addr.sin_port);

    // 2. 尝试启动服务端并断言失败
    SlowHandler handler;
    TcpServer server("127.0.0.1", occupied_port, handler);

    // start() 内部 bindAddress 会失败并主动调 stop()
    EXPECT_FALSE(server.start());

    // 即使启动失败，再次 stop 也是安全的
    EXPECT_NO_THROW({ server.stop(); });

    close(dummy_fd);
}

// 验证：有活跃连接及正在处理的业务请求时，依然能平滑退出
TEST(TcpServerShutdownTest, ShutdownWhileProcessingRequests) {
    SlowHandler handler;
    TcpServer server("127.0.0.1", 0, handler);

    std::thread server_thread([&]() { server.start(); });

    // 等待服务监听就绪并获取动态端口
    uint16_t server_port = 0;
    for (int i = 0; i < 50; ++i) {
        server_port = server.getPort();
        if (server_port != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(server_port, 0) << "Server failed to bind to a dynamic port";

    // 模拟客户端发起连接
    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

    ASSERT_EQ(connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)), 0);

    // 发送数据给服务端（这里会激活 epoll，促使 worker 取出任务并休眠）
    // 实际根据您的 chat::PacketCodec，可能需要构造真实有效包以完整进入 SlowHandler
    const char* dummy_msg = "test_request\n";
    const auto dummy_msg_len = static_cast<ssize_t>(strlen(dummy_msg));
    ASSERT_EQ(send(client_fd, dummy_msg, strlen(dummy_msg), MSG_NOSIGNAL), dummy_msg_len);

    // 确保 I/O 线程收到事件并抛给 ThreadPool
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 此时 worker 正在沉睡 100ms（业务处理中），立刻触发停机
    // 该调用应当做到：
    // 1. 设置 stopping_ = true 并退出 epoll
    // 2. 将此 tcp 连接断开，触发 client_fd = EOF
    // 3. 阻塞等待 worker 苏醒并完成处理
    // 4. 清理残留 eventfd 事件
    server.stop();

    // 确保服务端进程可以干净利落地退出
    if (server_thread.joinable()) {
        server_thread.join();
    }

    // 验证：服务端已经单方面踢掉了客户端 (recv 收到 0)
    char buff[16];
    ssize_t n = recv(client_fd, buff, sizeof(buff), MSG_DONTWAIT);
    EXPECT_EQ(n, 0);  // 正常被服务端 close

    close(client_fd);

    // 能顺利到达这里证明死锁、悬空fd操作、崩溃均未发生。
}

class ShutdownOrderHandler : public IMessageHandler {
   public:
    HandleResult handle(const std::string& request, const RequestContext& context) override {
        (void)request;
        (void)context;
        return {};
    }

    void onConnectionClosed(chat::ConnectionId conn_id) override {
        (void)conn_id;
        std::lock_guard<std::mutex> lock(mutex);
        cleanup_thread = std::this_thread::get_id();
        events.push_back("session_cleanup");
    }

    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        (void)conn_id;
        (void)user_id;
        return false;
    }

    std::mutex mutex;
    std::vector<std::string> events;
    std::thread::id cleanup_thread;
};

TEST(TcpServerShutdownTest, StopsExternalSourceBeforeWorkerCleanupAndWaitsForCleanup) {
    ShutdownOrderHandler handler;
    TcpServerTimeoutOptions options;
    options.scan_interval_ms = 10;
    TcpServer server("127.0.0.1", 0, handler, options);
    server.setPreShutdownHook([&handler]() {
        std::lock_guard<std::mutex> lock(handler.mutex);
        handler.events.push_back("push_stream_stop");
    });

    std::thread::id io_thread_id;
    std::thread server_thread([&]() {
        io_thread_id = std::this_thread::get_id();
        server.start();
    });

    uint16_t server_port = 0;
    for (int i = 0; i < 50; ++i) {
        server_port = server.getPort();
        if (server_port != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(server_port, 0);

    int client_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(client_fd, 0);
    struct sockaddr_in server_addr {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    ASSERT_EQ(connect(client_fd, reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr)), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    server.stop();
    server_thread.join();

    {
        std::lock_guard<std::mutex> lock(handler.mutex);
        ASSERT_EQ(handler.events.size(), 2u);
        EXPECT_EQ(handler.events[0], "push_stream_stop");
        EXPECT_EQ(handler.events[1], "session_cleanup");
        EXPECT_NE(handler.cleanup_thread, io_thread_id);
    }

    char byte = '\0';
    EXPECT_EQ(recv(client_fd, &byte, 1, MSG_DONTWAIT), 0);
    close(client_fd);
}

// 模拟产生 push 且可调控 isConnectionBoundToUser 返回值的测试 Handler
class PushTestHandler : public IMessageHandler {
   public:
    std::atomic<bool> bound_result{true};
    std::atomic<bool> push_checked{false};

    HandleResult handle(const std::string& request, const RequestContext& context) override {
        (void)request;
        HandleResult result;
        result.response = "ack";

        OutboundMessage push;
        push.target_conn_id = context.conn_id;
        push.target_user_id = 999;
        push.payload = "pushed_msg";
        result.pushes.push_back(push);
        return result;
    }

    void onConnectionClosed(uint64_t conn_id) override {}
    void applyBindSession(uint64_t conn_id, const chat::ConnectionSession& session) override {}
    void applyUnbindSession(uint64_t conn_id) override {}

    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        (void)conn_id;
        (void)user_id;
        push_checked = true;
        return bound_result.load();
    }
};

// 验证：当目标连接 ID 存在但 target_user_id 与当前 Session 不匹配时，Push 消息被丢弃且不发往客户端
TEST(TcpServerShutdownTest, PushDiscardedWhenConnectionRebound) {
    PushTestHandler handler;
    TcpServer server("127.0.0.1", 0, handler);

    std::thread server_thread([&]() { server.start(); });

    // 等待服务监听就绪并获取动态端口
    uint16_t server_port = 0;
    for (int i = 0; i < 50; ++i) {
        server_port = server.getPort();
        if (server_port != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_NE(server_port, 0) << "Server failed to bind to a dynamic port";

    // 1. 测试 bound_result = true，推送被成功投递
    {
        handler.bound_result = true;
        handler.push_checked = false;

        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client_fd, 0);
        struct sockaddr_in server_addr {};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        ASSERT_EQ(connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)), 0);

        const char* msg = "trigger\n";
        send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);

        // 等待 I/O 线程和 worker 线程处理完成
        for (int i = 0; i < 100; ++i) {
            if (handler.push_checked.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(handler.push_checked.load());

        // 我们应该能从 socket 读到 "ack\n" 和 "pushed_msg\n"
        std::string received;
        char buff[128];
        while (received.find("pushed_msg\n") == std::string::npos) {
            ssize_t n = recv(client_fd, buff, sizeof(buff) - 1, 0);
            ASSERT_GT(n, 0);
            buff[n] = '\0';
            received.append(buff);
        }

        EXPECT_NE(received.find("ack\n"), std::string::npos);
        EXPECT_NE(received.find("pushed_msg\n"), std::string::npos);

        close(client_fd);
    }

    // 2. 测试 bound_result = false，推送被丢弃
    {
        handler.bound_result = false;
        handler.push_checked = false;

        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(client_fd, 0);
        struct sockaddr_in server_addr {};
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);

        ASSERT_EQ(connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)), 0);

        const char* msg = "trigger\n";
        send(client_fd, msg, strlen(msg), MSG_NOSIGNAL);

        // 等待 I/O 线程和 worker 线程处理完成
        for (int i = 0; i < 100; ++i) {
            if (handler.push_checked.load())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_TRUE(handler.push_checked.load());

        // 稍微等待一下确保推送如果在底层被发送，会被写入内核缓冲区
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // 我们应该能读到 "ack\n"，但读不到 "pushed_msg\n"
        std::string received;
        char buff[128];
        while (received.find("ack\n") == std::string::npos) {
            ssize_t n = recv(client_fd, buff, sizeof(buff) - 1, 0);
            ASSERT_GT(n, 0);
            buff[n] = '\0';
            received.append(buff);
        }

        EXPECT_NE(received.find("ack\n"), std::string::npos);
        // 确认没有收到 pushed_msg
        EXPECT_EQ(received.find("pushed_msg\n"), std::string::npos);

        // 使用 MSG_DONTWAIT 确认没有后续推送数据
        ssize_t n = recv(client_fd, buff, sizeof(buff), MSG_DONTWAIT);
        EXPECT_EQ(n, -1);
        EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

        close(client_fd);
    }

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
}
