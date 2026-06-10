#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "codec/packet_codec.h"
#include "common/error_code.h"
#include "nlohmann/json.hpp"

namespace {

constexpr const char* kServerIp = "127.0.0.1";
constexpr uint16_t kAuthTestPort = 18080;

class ServerProcess {
   public:
    ServerProcess() {
        log_path_ = "/tmp/linux_server_auth_test_" + std::to_string(getpid()) + ".log";
        std::remove(log_path_.c_str());
    }

    ~ServerProcess() {
        Stop();
        std::remove(log_path_.c_str());
    }

    void Start() {
        assert(pid_ == -1);

        pid_ = fork();
        assert(pid_ >= 0);

        if (pid_ == 0) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            const int log_fd = open(log_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            assert(log_fd >= 0);
            assert(dup2(log_fd, STDERR_FILENO) >= 0);
            close(log_fd);
            execl(AUTH_TEST_SERVER_BINARY_PATH, AUTH_TEST_SERVER_BINARY_PATH, static_cast<char*>(nullptr));
            _exit(127);
        }

        WaitUntilReady();
    }

    void Stop() {
        if (pid_ <= 0) {
            return;
        }

        kill(pid_, SIGTERM);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }

    std::string ReadLog() const {
        std::ifstream input(log_path_);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

   private:
    void WaitUntilReady() {
        for (int i = 0; i < 50; ++i) {
            const int fd = socket(AF_INET, SOCK_STREAM, 0);
            assert(fd >= 0);

            sockaddr_in server_addr{};
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(kAuthTestPort);
            const int pton_ok = inet_pton(AF_INET, kServerIp, &server_addr.sin_addr);
            assert(pton_ok == 1);

            const int rc = connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
            close(fd);

            if (rc == 0) {
                return;
            }

            usleep(100 * 1000);
        }

        assert(false && "auth test server did not become ready in time");
    }

    pid_t pid_ = -1;
    std::string log_path_;
};

int ConnectToServer() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(kAuthTestPort);
    const int pton_ok = inet_pton(AF_INET, kServerIp, &server_addr.sin_addr);
    assert(pton_ok == 1);

    const int rc = connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
    assert(rc == 0);
    return fd;
}

nlohmann::json SendAndReceiveOnSocket(int fd, const std::string& request) {
    chat::PacketCodec codec;
    const std::string encoded_request = codec.encode(request);
    const ssize_t written = send(fd, encoded_request.data(), encoded_request.size(), 0);
    assert(written == static_cast<ssize_t>(encoded_request.size()));

    std::string response;
    char buffer[1024];
    while (response.find('\n') == std::string::npos) {
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        assert(n > 0);
        response.append(buffer, static_cast<size_t>(n));
    }

    return nlohmann::json::parse(response);
}

nlohmann::json SendAndReceive(const std::string& request) {
    const int fd = ConnectToServer();
    const nlohmann::json response = SendAndReceiveOnSocket(fd, request);
    close(fd);
    return response;
}

void ExpectCommonEnvelope(const nlohmann::json& resp, const std::string& msg_type, int seq, chat::ErrorCode code) {
    assert(resp.contains("msg_type"));
    assert(resp.contains("seq"));
    assert(resp.contains("code"));
    assert(resp.contains("message"));
    assert(resp.contains("data"));

    assert(resp["msg_type"].get<std::string>() == msg_type);
    assert(resp["seq"].get<int>() == seq);
    assert(resp["code"].get<int>() == static_cast<int>(code));
    assert(resp["data"].is_object());
}

void TestRegisterSuccessOverTcp() {
    const nlohmann::json resp = SendAndReceive(
        R"({"msg_type":"register","seq":1,"token":"","data":{"username":"bob","password":"123456","nickname":"Bob"}})");

    ExpectCommonEnvelope(resp, "register_resp", 1, chat::ErrorCode::OK);
    assert(resp["message"].get<std::string>() == "register success");
    assert(resp["data"]["user_id"].is_number_integer());
    assert(resp["data"]["user_id"].get<int>() > 0);
}

void TestRegisterDuplicateUsernameOverTcp() {
    const nlohmann::json resp = SendAndReceive(
        R"({"msg_type":"register","seq":2,"token":"","data":{"username":"alice","password":"123456","nickname":"AliceAgain"}})");

    ExpectCommonEnvelope(resp, "register_resp", 2, chat::ErrorCode::USER_ALREADY_EXISTS);
    assert(resp["message"].get<std::string>() == "username already exists");
}

void TestLoginSuccessOverTcp() {
    const nlohmann::json resp =
        SendAndReceive(R"({"msg_type":"login","seq":3,"token":"","data":{"username":"alice","password":"123456"}})");

    ExpectCommonEnvelope(resp, "login_resp", 3, chat::ErrorCode::OK);
    assert(resp["message"].get<std::string>() == "login success");
    assert(resp["data"]["user_id"].get<int>() == 10001);
    assert(resp["data"]["nickname"].get<std::string>() == "Alice");
    assert(resp["data"]["token"].get<std::string>() == "token_10001");
}

void TestLoginWrongPasswordOverTcp() {
    const nlohmann::json resp =
        SendAndReceive(R"({"msg_type":"login","seq":4,"token":"","data":{"username":"alice","password":"wrong"}})");

    ExpectCommonEnvelope(resp, "login_resp", 4, chat::ErrorCode::INVALID_CREDENTIALS);
    assert(resp["message"].get<std::string>() == "invalid username or password");
}

void TestWhoAmIAfterLoginAndLogoutOnSameConnection() {
    const int fd = ConnectToServer();

    const nlohmann::json login_resp = SendAndReceiveOnSocket(
        fd, R"({"msg_type":"login","seq":5,"token":"","data":{"username":"alice","password":"123456"}})");
    ExpectCommonEnvelope(login_resp, "login_resp", 5, chat::ErrorCode::OK);

    const nlohmann::json whoami_resp =
        SendAndReceiveOnSocket(fd, R"({"msg_type":"whoami","seq":6,"token":"","data":{}})");
    ExpectCommonEnvelope(whoami_resp, "whoami_resp", 6, chat::ErrorCode::OK);
    assert(whoami_resp["data"]["user_id"].get<int>() == 10001);
    assert(whoami_resp["data"]["username"].get<std::string>() == "alice");
    assert(whoami_resp["data"]["token"].get<std::string>() == "token_10001");

    const nlohmann::json logout_resp =
        SendAndReceiveOnSocket(fd, R"({"msg_type":"logout","seq":7,"token":"","data":{}})");
    ExpectCommonEnvelope(logout_resp, "logout_resp", 7, chat::ErrorCode::OK);
    assert(logout_resp["message"].get<std::string>() == "logout success");

    const nlohmann::json whoami_after_logout =
        SendAndReceiveOnSocket(fd, R"({"msg_type":"whoami","seq":8,"token":"","data":{}})");
    ExpectCommonEnvelope(whoami_after_logout, "whoami_resp", 8, chat::ErrorCode::USER_NOT_FOUND);
    assert(whoami_after_logout["message"].get<std::string>() == "user not logged in");

    close(fd);
}

void TestSendMessageOverTcp() {
    // Connect both Alice and Bob to the server
    const int fd_alice = ConnectToServer();
    const int fd_bob = ConnectToServer();

    // Log Alice in
    const nlohmann::json login_alice = SendAndReceiveOnSocket(
        fd_alice, R"({"msg_type":"login","seq":10,"token":"","data":{"username":"alice","password":"123456"}})");
    ExpectCommonEnvelope(login_alice, "login_resp", 10, chat::ErrorCode::OK);

    // Log Bob in (Bob was registered in TestRegisterSuccessOverTcp with username bob and password 123456)
    const nlohmann::json login_bob = SendAndReceiveOnSocket(
        fd_bob, R"({"msg_type":"login","seq":11,"token":"","data":{"username":"bob","password":"123456"}})");
    ExpectCommonEnvelope(login_bob, "login_resp", 11, chat::ErrorCode::OK);
    const int bob_user_id = login_bob["data"]["user_id"].get<int>();

    // Alice sends message to Bob
    const std::string send_msg_request =
        R"({"msg_type":"send_message","seq":12,"token":"","data":{"client_msg_id":"cmsg_tcp_12","to_user_id":)" +
        std::to_string(bob_user_id) + R"(,"content":"hello bob"}})";

    const nlohmann::json ack_resp = SendAndReceiveOnSocket(fd_alice, send_msg_request);

    // Verify Alice receives the send_message_resp ACK
    ExpectCommonEnvelope(ack_resp, "send_message_resp", 12, chat::ErrorCode::OK);
    assert(ack_resp["data"]["to_user_id"].get<int>() == bob_user_id);
    assert(!ack_resp["data"]["message_id"].get<std::string>().empty());
    assert(!ack_resp["data"]["conversation_id"].get<std::string>().empty());
    assert(ack_resp["data"]["status"].get<int>() == 0);
    assert(ack_resp["data"]["created_at"].is_number_integer());

    // Verify Bob receives the message_push
    auto ReceiveOnSocket = [](int fd) {
        std::string response;
        char buffer[1024];
        while (response.find('\n') == std::string::npos) {
            const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
            assert(n > 0);
            response.append(buffer, static_cast<size_t>(n));
        }
        return nlohmann::json::parse(response);
    };

    const nlohmann::json push_resp = ReceiveOnSocket(fd_bob);
    ExpectCommonEnvelope(push_resp, "message_push", 0, chat::ErrorCode::OK);
    assert(!push_resp["data"]["message_id"].get<std::string>().empty());
    assert(!push_resp["data"]["conversation_id"].get<std::string>().empty());
    assert(push_resp["data"]["from_user_id"].get<int>() == 10001);
    assert(push_resp["data"]["from_username"].get<std::string>() == "alice");
    assert(push_resp["data"]["to_user_id"].get<int>() == bob_user_id);
    assert(push_resp["data"]["content"].get<std::string>() == "hello bob");
    assert(push_resp["data"]["created_at"].is_number_integer());
    assert(push_resp["data"].contains("server_time"));

    close(fd_alice);
    close(fd_bob);
}

void TestOfflineMessagePullOverTcp() {
    const int fd_alice = ConnectToServer();

    const nlohmann::json login_alice = SendAndReceiveOnSocket(
        fd_alice, R"({"msg_type":"login","seq":30,"token":"","data":{"username":"alice","password":"123456"}})");
    ExpectCommonEnvelope(login_alice, "login_resp", 30, chat::ErrorCode::OK);

    constexpr int kBobUserId = 10002;
    const std::string send_msg_request =
        R"({"msg_type":"send_message","seq":31,"token":"","data":{"client_msg_id":"cmsg_tcp_offline_31","to_user_id":)" +
        std::to_string(kBobUserId) + R"(,"content":"offline hello bob"}})";
    const nlohmann::json ack_resp = SendAndReceiveOnSocket(fd_alice, send_msg_request);
    ExpectCommonEnvelope(ack_resp, "send_message_resp", 31, chat::ErrorCode::OK);
    const std::string message_id = ack_resp["data"]["message_id"].get<std::string>();
    assert(!message_id.empty());
    close(fd_alice);

    const int fd_bob = ConnectToServer();
    const nlohmann::json login_bob = SendAndReceiveOnSocket(
        fd_bob, R"({"msg_type":"login","seq":32,"token":"","data":{"username":"bob","password":"123456"}})");
    ExpectCommonEnvelope(login_bob, "login_resp", 32, chat::ErrorCode::OK);
    assert(login_bob["data"]["user_id"].get<int>() == kBobUserId);

    const nlohmann::json pull_resp = SendAndReceiveOnSocket(
        fd_bob, R"({"msg_type":"pull_offline_messages","seq":33,"token":"","data":{"limit":10}})");
    ExpectCommonEnvelope(pull_resp, "pull_offline_messages_resp", 33, chat::ErrorCode::OK);
    assert(pull_resp["data"]["messages"].is_array());
    bool found_offline_message = false;
    for (const auto& pulled : pull_resp["data"]["messages"]) {
        if (pulled["message_id"].get<std::string>() != message_id) {
            continue;
        }
        assert(pulled["from_user_id"].get<int>() == 10001);
        assert(pulled["to_user_id"].get<int>() == kBobUserId);
        assert(pulled["content"].get<std::string>() == "offline hello bob");
        found_offline_message = true;
    }
    assert(found_offline_message);

    const nlohmann::json second_pull_resp = SendAndReceiveOnSocket(
        fd_bob, R"({"msg_type":"pull_offline_messages","seq":34,"token":"","data":{"limit":10}})");
    ExpectCommonEnvelope(second_pull_resp, "pull_offline_messages_resp", 34, chat::ErrorCode::OK);
    assert(second_pull_resp["data"]["messages"].is_array());
    assert(second_pull_resp["data"]["messages"].empty());

    close(fd_bob);
}

}  // namespace

int main() {
    ServerProcess server;
    server.Start();

    TestRegisterSuccessOverTcp();
    TestRegisterDuplicateUsernameOverTcp();
    TestLoginSuccessOverTcp();
    TestLoginWrongPasswordOverTcp();
    TestWhoAmIAfterLoginAndLogoutOnSameConnection();
    TestSendMessageOverTcp();
    TestOfflineMessagePullOverTcp();

    server.Stop();
    const std::string log = server.ReadLog();
    const std::vector<std::string> forbidden = {
        "123456",         "wrong",
        "token_",         std::to_string(std::hash<std::string>{}("123456")),
        "hello bob",      "offline hello bob",
        R"({"msg_type")", "SELECT ",
        "INSERT INTO",    "UPDATE ",
        "DELETE FROM",    "START TRANSACTION",
        "COMMIT",         "ROLLBACK",
    };
    for (const auto& value : forbidden) {
        assert(log.find(value) == std::string::npos);
    }

    std::cout << "[PASS] auth integration tests passed\n";
    return 0;
}
