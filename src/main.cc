#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include "app/main_runner.h"
#include "config/redis_config.h"
#include "db/db_pool.h"
#include "db/message_repository.h"
#include "db/user_repository.h"
#include "handler/message_handler.h"
#include "net/TcpServer.h"
#include "redis/redis_client.h"
#include "redis/redis_pool.h"
#include "server/redis_session_store.h"
#include "server/session_manager.h"
#include "service/chat_service.h"
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

    const chat::RedisConfigResult redis_config_result = chat::LoadRedisConfigFromEnv();
    if (!redis_config_result.ok()) {
        std::cerr << "Failed to load Redis config: " << redis_config_result.message << std::endl;
        return 1;
    }

    std::unique_ptr<chat::RedisPool> redis_pool;
    std::unique_ptr<chat::RedisClient> redis_client;
    std::unique_ptr<chat::RedisSessionStore> session_store;
    if (redis_config_result.config.enabled) {
        redis_pool = std::make_unique<chat::RedisPool>(redis_config_result.config);
        const chat::RedisPoolInitResult redis_init = redis_pool->Init();
        if (!redis_init.ok()) {
            std::cerr << "Failed to initialize RedisPool: " << redis_init.message << std::endl;
            return 1;
        }
        redis_client = std::make_unique<chat::RedisClient>(redis_pool.get());
        session_store = std::make_unique<chat::RedisSessionStore>(redis_client.get(), redis_config_result.config);
    }

    // 2. 依赖注入：Pool -> Repository/Store -> Service -> MessageHandler
    chat::UserRepository user_repo(&db_pool);
    chat::MessageRepository message_repo(&db_pool);
    chat::SessionManager session_manager;
    chat::UserService user_service(user_repo, session_manager, session_store.get());
    chat::ChatService chat_service(session_manager, message_repo, user_repo);
    chat::MessageHandler handler(user_service, chat_service);

    // 3. 启动服务器
    TcpServer server("127.0.0.1", 8080, handler);
    return RunMain([&]() { return server.start(); });
}
