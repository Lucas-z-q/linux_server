#include <string>
#include <unordered_map>

#include "app/main_runner.h"
#include "db/user_repository.h"
#include "fake_message_repository.h"
#include "handler/message_handler.h"
#include "model/user_record.h"
#include "net/TcpServer.h"
#include "server/session_manager.h"
#include "service/chat_service.h"
#include "service/user_service.h"

namespace {

constexpr uint16_t kAuthTestPort = 18080;

chat::UserRecord MakeUserRecord(chat::UserId id, const std::string& username, const std::string& password,
                                const std::string& nickname) {
    chat::UserRecord record;
    record.id = id;
    record.username = username;
    record.password_hash = std::to_string(std::hash<std::string>{}(password));
    record.nickname = nickname;
    record.status = 1;
    record.created_at = "2026-01-01 00:00:00";
    record.updated_at = "2026-01-01 00:00:00";
    return record;
}

class InMemoryUserRepository : public chat::IUserRepository {
   public:
    InMemoryUserRepository() {
        const chat::UserRecord alice = MakeUserRecord(10001, "alice", "123456", "Alice");
        users_by_name_.emplace(alice.username, alice);
        next_user_id_ = 10002;
    }

    chat::FindUserResult findByUsername(const std::string& username) override {
        const auto it = users_by_name_.find(username);
        if (it == users_by_name_.end()) {
            return {.status = chat::RepositoryStatus::kNotFound};
        }
        return {.status = chat::RepositoryStatus::kOk, .user = it->second};
    }

    chat::FindUserResult findById(chat::UserId user_id) override {
        for (const auto& entry : users_by_name_) {
            if (entry.second.id == user_id) {
                return {.status = chat::RepositoryStatus::kOk, .user = entry.second};
            }
        }
        return {.status = chat::RepositoryStatus::kNotFound};
    }

    chat::CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                      const std::string& nickname) override {
        if (users_by_name_.find(username) != users_by_name_.end()) {
            return {.status = chat::RepositoryStatus::kDuplicate};
        }

        chat::UserRecord record;
        record.id = next_user_id_++;
        record.username = username;
        record.password_hash = password_hash;
        record.nickname = nickname;
        record.status = 1;
        record.created_at = "2026-01-01 00:00:00";
        record.updated_at = "2026-01-01 00:00:00";
        users_by_name_.emplace(username, record);
        return {.status = chat::RepositoryStatus::kOk, .user_id = record.id};
    }

   private:
    std::unordered_map<std::string, chat::UserRecord> users_by_name_;
    chat::UserId next_user_id_ = 1;
};

}  // namespace

int main() {
    InMemoryUserRepository repository;
    FakeMessageRepository message_repository;
    chat::SessionManager session_manager;
    chat::UserService user_service(repository, session_manager);
    chat::ChatService chat_service(session_manager, message_repository, repository);
    chat::MessageHandler handler(user_service, chat_service);
    TcpServerTimeoutOptions timeout_options;
    timeout_options.idle_timeout_ms = 5000;
    timeout_options.heartbeat_timeout_ms = 500;
    timeout_options.scan_interval_ms = 20;
    TcpServer server("127.0.0.1", kAuthTestPort, handler, timeout_options);

    return RunMain([&]() { return server.start(); });
}
