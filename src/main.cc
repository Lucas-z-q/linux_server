#include <iostream>
#include <variant>

#include "app/main_runner.h"
#include "config/config_loader.h"
#include "config/server_config.h"
#include "db/db_pool.h"
#include "db/message_repository.h"
#include "db/user_repository.h"
#include "handler/message_handler.h"
#include "net/TcpServer.h"
#include "server/session_manager.h"
#include "service/chat_service.h"
#include "service/user_service.h"

/**
 * @file main.cc
 * @brief Entry point for the chat server.
 *
 * 启动流程：
 *   1. 解析 --config 参数，加载并校验 ServerConfig。
 *   2. 使用配置初始化 DbPool（含连接池参数）。
 *   3. 依赖注入：Repository -> Service -> MessageHandler。
 *   4. 启动 TcpServer（使用配置中的 IP、端口和推送超时）。
 */
int main(int argc, char* argv[]) {
    // 1. 加载配置
    std::string config_path = chat::ParseConfigPathArg(argc, argv);
    chat::ConfigResult config_result = chat::ConfigLoader::Load(config_path);
    if (std::holds_alternative<chat::ConfigError>(config_result)) {
        std::cerr << "Config error: " << std::get<chat::ConfigError>(config_result).message << std::endl;
        return 1;
    }
    const chat::ServerConfig& cfg = std::get<chat::ServerConfig>(config_result);

    // 输出脱敏配置（用于启动确认日志）
    std::cout << "Starting server with config:\n" << cfg.ToSafeString() << std::endl;

    // 2. 初始化数据库连接池（传入连接池配置使其真正生效）
    chat::DbPool db_pool(cfg.mysql, cfg.mysql_pool);
    auto db_init_res = db_pool.init();
    if (!db_init_res.success) {
        std::cerr << "Failed to initialize DbPool: " << db_init_res.message
                  << " (mysql_error_code=" << db_init_res.mysql_error_code << ")" << std::endl;
        return 1;
    }

    // 3. 依赖注入：DbPool -> Repository -> Service -> MessageHandler
    chat::UserRepository user_repo(&db_pool);
    chat::MessageRepository message_repo(&db_pool);
    chat::SessionManager session_manager;
    chat::UserService user_service(user_repo, session_manager);
    chat::ChatService chat_service(session_manager, message_repo, user_repo);
    chat::MessageHandler handler(user_service, chat_service);

    // 4. 启动服务器
    TcpServer server(cfg.server.listen_ip, cfg.server.listen_port, handler);
    return RunMain([&]() { return server.start(); });
}
