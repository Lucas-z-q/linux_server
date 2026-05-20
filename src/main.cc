#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "app/main_runner.h"
#include "db/db_pool.h"
#include "db/user_repository.h"
#include "handler/message_handler.h"
#include "net/TcpServer.h"
#include "server/session_manager.h"
#include "service/user_service.h"

/**
 * @file main.cc
 * @brief Entry point for starting the TCP echo server.
 */

// 从环境变量装载数据库配置
chat::DbConfig LoadDbConfigFromEnv() {
    chat::DbConfig config;
    if (const char* host = std::getenv("CHAT_DB_HOST"))
        config.host = host;
    else
        config.host = "127.0.0.1";

    if (const char* port = std::getenv("CHAT_DB_PORT"))
        config.port = std::atoi(port);
    else
        config.port = 3306;

    if (const char* user = std::getenv("CHAT_DB_USER"))
        config.username = user;
    else
        config.username = "root";

    if (const char* pwd = std::getenv("CHAT_DB_PASSWORD"))
        config.password = pwd;
    else
        config.password = "123456";

    if (const char* db = std::getenv("CHAT_DB_NAME"))
        config.database = db;
    else
        config.database = "chat";

    return config;
}

/**
 * @brief Creates and runs the TCP server.
 * @return 0 on success, non-zero on startup failure.
 */
int main() {
    // 1. 初始化数据库配置与连接池
    chat::DbConfig db_config = LoadDbConfigFromEnv();
    chat::DbPool db_pool(db_config);
    auto db_init_res = db_pool.init();
    if (!db_init_res.success) {
        std::cerr << "Failed to initialize DbPool: " << db_init_res.message
                  << " (mysql_error_code=" << db_init_res.mysql_error_code << ")" << std::endl;
        return 1;
    }

    // 2. 依赖注入：DbPool -> UserRepository -> UserService -> MessageHandler
    chat::UserRepository user_repo(&db_pool);
    chat::SessionManager session_manager;
    chat::UserService user_service(user_repo, session_manager);
    chat::MessageHandler handler(user_service);

    // 3. 启动服务器
    TcpServer server("127.0.0.1", 8080, handler);
    return RunMain([&]() { return server.start(); });
}
