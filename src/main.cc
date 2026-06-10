#include <cstddef>
#include <string>
#include <variant>

#include "app/main_runner.h"
#include "common/logger.h"
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
 *   2. 初始化 Logger，并确保其晚于业务组件关闭。
 *   3. 使用配置初始化 DbPool（含连接池参数）。
 *   4. 依赖注入：Repository -> Service -> MessageHandler。
 *   5. 启动 TcpServer（使用配置中的 IP、端口和推送超时）。
 */
namespace {

class LoggerShutdownGuard {
   public:
    ~LoggerShutdownGuard() { chat::Logger::Instance().Shutdown(); }
};

}  // namespace

int main(int argc, char* argv[]) {
    // 1. 加载配置
    std::string config_path = chat::ParseConfigPathArg(argc, argv);
    chat::ConfigResult config_result = chat::ConfigLoader::Load(config_path);
    if (std::holds_alternative<chat::ConfigError>(config_result)) {
        LOG_ERROR("Main") << "config error: " << std::get<chat::ConfigError>(config_result).message;
        return 1;
    }
    const chat::ServerConfig& cfg = std::get<chat::ServerConfig>(config_result);

    chat::LogLevel min_level = chat::LogLevel::kInfo;
    if (!chat::ParseLogLevel(cfg.log.level, &min_level)) {
        LOG_ERROR("Main") << "invalid configured log level";
        return 1;
    }

    constexpr std::size_t kBytesPerMegabyte = 1024 * 1024;
    chat::LoggerOptions logger_options;
    logger_options.min_level = min_level;
    logger_options.console = cfg.log.console;
    logger_options.file_path = cfg.log.file_path;
    logger_options.max_file_size_bytes = cfg.log.max_size_mb * kBytesPerMegabyte;
    logger_options.max_files = cfg.log.max_files;
    logger_options.async = cfg.log.async;
    if (!chat::Logger::Instance().Initialize(logger_options)) {
        chat::LoggerOptions fallback_options;
        fallback_options.console = true;
        fallback_options.file_path.clear();
        fallback_options.async = false;
        chat::Logger::Instance().Initialize(fallback_options);
        LOG_ERROR("Main") << "logger initialization failed";
        return 1;
    }
    LoggerShutdownGuard logger_shutdown_guard;

    LOG_INFO("Main") << "starting server with config=" << cfg.ToSafeString();

    // 2. 初始化数据库连接池（传入连接池配置使其真正生效）
    chat::DbPool db_pool(cfg.mysql, cfg.mysql_pool);
    auto db_init_res = db_pool.init();
    if (!db_init_res.success) {
        LOG_ERROR("Main") << "DbPool initialization failed error=" << chat::DbPoolErrorToString(db_init_res.error)
                          << " mysql_errno=" << db_init_res.mysql_error_code;
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
