#include "app/main_runner.h"
#include "db/user_repository.h"
#include "handler/message_handler.h"
#include "net/TcpServer.h"
#include "server/session_manager.h"
#include "service/user_service.h"

namespace {

class FailingUserRepository : public chat::IUserRepository {
   public:
    chat::FindUserResult findByUsername(const std::string&) override {
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    chat::FindUserResult findById(chat::UserId) override {
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    chat::CreateUserResult createUser(const std::string&,
                                      const std::string&,
                                      const std::string&) override {
        return {.status = chat::RepositoryStatus::kInsertFailed};
    }
};

}  // namespace

int main() {
    FailingUserRepository repository;
    chat::SessionManager session_manager;
    chat::UserService user_service(repository, session_manager);
    chat::MessageHandler handler(user_service);
    TcpServer server("127.0.0.1", 8080, handler);

    return RunMain([&]() { return server.start(); });
}
