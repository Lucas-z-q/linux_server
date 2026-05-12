#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cassert>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "common/error_code.h"
#include "codec/packet_codec.h"

namespace {

constexpr const char* kServerIp = "127.0.0.1";
constexpr uint16_t kServerPort = 8080;

class ServerProcess {
 public:
  ServerProcess() = default;

  ~ServerProcess() { Stop(); }

  void Start() {
    assert(pid_ == -1);

    pid_ = fork();
    assert(pid_ >= 0);

    if (pid_ == 0) {
      prctl(PR_SET_PDEATHSIG, SIGTERM);
      execl(SERVER_BINARY_PATH, SERVER_BINARY_PATH, static_cast<char*>(nullptr));
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

 private:
  void WaitUntilReady() {
    for (int i = 0; i < 50; ++i) {
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      assert(fd >= 0);

      sockaddr_in server_addr{};
      server_addr.sin_family = AF_INET;
      server_addr.sin_port = htons(kServerPort);
      const int pton_ok = inet_pton(AF_INET, kServerIp, &server_addr.sin_addr);
      assert(pton_ok == 1);

      const int rc = connect(fd, reinterpret_cast<sockaddr*>(&server_addr),
                             sizeof(server_addr));
      close(fd);

      if (rc == 0) {
        return;
      }

      usleep(100 * 1000);
    }

    assert(false && "server did not become ready in time");
  }

  pid_t pid_ = -1;
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
  server_addr.sin_port = htons(kServerPort);
  const int pton_ok = inet_pton(AF_INET, kServerIp, &server_addr.sin_addr);
  assert(pton_ok == 1);

  const int rc =
      connect(fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr));
  assert(rc == 0);
  return fd;
}

nlohmann::json SendAndReceiveOnSocket(int fd, const std::string& request) {
  chat::PacketCodec codec;
  const std::string encoded_request = codec.encode(request);
  const ssize_t written =
      send(fd, encoded_request.data(), encoded_request.size(), 0);
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

void ExpectCommonEnvelope(const nlohmann::json& resp, const std::string& msg_type,
                          int seq, chat::ErrorCode code) {
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

void TestHeartbeatRouteOverTcp() {
  const nlohmann::json resp =
      SendAndReceive(R"({"msg_type":"heartbeat","seq":1,"token":"","data":{}})");

  ExpectCommonEnvelope(resp, "heartbeat_resp", 1, chat::ErrorCode::OK);
  assert(resp["data"].contains("server_time"));
  assert(resp["data"]["server_time"].is_number_integer());
}

void TestUnknownRouteOverTcp() {
  const nlohmann::json resp =
      SendAndReceive(R"({"msg_type":"chat","seq":2,"token":"","data":{}})");

  ExpectCommonEnvelope(resp, "chat_resp", 2,
                       chat::ErrorCode::UNKNOWN_MESSAGE_TYPE);
}

void TestLoginValidationOverTcp() {
  const nlohmann::json resp =
      SendAndReceive(R"({"msg_type":"login","seq":3,"token":"","data":{"username":"alice"}})");

  ExpectCommonEnvelope(resp, "login_resp", 3,
                       chat::ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("password") !=
         std::string::npos);
}

void TestLoginDbQueryFailureOverTcp() {
  const nlohmann::json resp = SendAndReceive(
      R"({"msg_type":"login","seq":4,"token":"","data":{"username":"alice","password":"123456"}})");

  ExpectCommonEnvelope(resp, "login_resp", 4,
                       chat::ErrorCode::DB_QUERY_FAILED);
  assert(resp["message"].get<std::string>() == "query user failed");
}

void TestRegisterDbQueryFailureOverTcp() {
  const nlohmann::json resp = SendAndReceive(
      R"({"msg_type":"register","seq":5,"token":"","data":{"username":"alice","password":"123456","nickname":"Alice"}})");

  ExpectCommonEnvelope(resp, "register_resp", 5,
                       chat::ErrorCode::DB_QUERY_FAILED);
  assert(resp["message"].get<std::string>() == "query user failed");
}

void TestInvalidJsonOverTcp() {
  const nlohmann::json resp =
      SendAndReceive(R"({"msg_type":"login","seq":8,"data":)");

  ExpectCommonEnvelope(resp, "_resp", 0, chat::ErrorCode::INVALID_PARAM);
  assert(resp["message"].get<std::string>().find("JSON") != std::string::npos);
}

void TestWhoAmIAndLogoutRequireLoginOnSameConnection() {
  const int fd = ConnectToServer();

  const nlohmann::json whoami_resp = SendAndReceiveOnSocket(
      fd, R"({"msg_type":"whoami","seq":6,"token":"","data":{}})");
  ExpectCommonEnvelope(whoami_resp, "whoami_resp", 6,
                       chat::ErrorCode::USER_NOT_FOUND);
  assert(whoami_resp["message"].get<std::string>() == "user not logged in");

  const nlohmann::json logout_resp = SendAndReceiveOnSocket(
      fd, R"({"msg_type":"logout","seq":7,"token":"","data":{}})");
  ExpectCommonEnvelope(logout_resp, "logout_resp", 7,
                       chat::ErrorCode::USER_NOT_FOUND);
  assert(logout_resp["message"].get<std::string>() == "user not logged in");

  close(fd);
}

}  // namespace

int main() {
  ServerProcess server;
  server.Start();

  TestHeartbeatRouteOverTcp();
  TestUnknownRouteOverTcp();
  TestLoginValidationOverTcp();
  TestLoginDbQueryFailureOverTcp();
  TestRegisterDbQueryFailureOverTcp();
  TestInvalidJsonOverTcp();
  TestWhoAmIAndLogoutRequireLoginOnSameConnection();

  std::cout << "[PASS] server integration tests passed\n";
  return 0;
}
