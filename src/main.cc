#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>

#include "app/main_runner.h"
#include "cache/cached_user_repository.h"
#include "cache/message_dedup_cache.h"
#include "cache/redis_rate_limiter.h"
#include "common/logger.h"
#include "config/config_loader.h"
#include "config/server_config.h"
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
#include "stream/redis_push_stream.h"

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

    std::unique_ptr<chat::RedisPool> redis_pool;
    std::unique_ptr<chat::RedisClient> redis_client;
    std::unique_ptr<chat::RedisSessionStore> session_store;
    std::unique_ptr<chat::RedisRateLimiter> rate_limiter;
    std::unique_ptr<chat::MessageDedupCache> dedup_cache;
    std::unique_ptr<chat::RedisPushStream> push_stream;
    if (cfg.redis.enabled) {
        redis_pool = std::make_unique<chat::RedisPool>(cfg.redis);
        const chat::RedisPoolInitResult redis_init = redis_pool->Init();
        if (!redis_init.ok()) {
            LOG_ERROR("Main") << "RedisPool initialization failed error=" << redis_init.message;
            return 1;
        }
        redis_client = std::make_unique<chat::RedisClient>(redis_pool.get());
        session_store = std::make_unique<chat::RedisSessionStore>(redis_client.get(), cfg.redis);
        rate_limiter = std::make_unique<chat::RedisRateLimiter>(redis_client.get(), cfg.redis);
        dedup_cache = std::make_unique<chat::MessageDedupCache>(redis_client.get(), cfg.redis);
        push_stream = std::make_unique<chat::RedisPushStream>(redis_client.get(), cfg.redis);
        if (!push_stream->Initialize()) {
            LOG_ERROR("Main") << "Redis push stream initialization failed";
            return 1;
        }
    }

    // 3. 依赖注入：Pool -> Repository/Store -> Service -> MessageHandler
    chat::UserRepository user_repo(&db_pool);
    std::unique_ptr<chat::CachedUserRepository> cached_user_repo;
    chat::IUserRepository* active_user_repo = &user_repo;
    if (redis_client) {
        cached_user_repo = std::make_unique<chat::CachedUserRepository>(&user_repo, redis_client.get(), cfg.redis);
        active_user_repo = cached_user_repo.get();
    }
    chat::MessageRepository message_repo(&db_pool);
    chat::SessionManager session_manager;
    chat::UserService user_service(*active_user_repo, session_manager, session_store.get(), rate_limiter.get(),
                                   cfg.redis);
    chat::ChatService chat_service(session_manager, message_repo, *active_user_repo, rate_limiter.get(),
                                   dedup_cache.get(), cfg.redis, session_store.get(), push_stream.get());
    chat::MessageHandler handler(user_service, chat_service);

    // 4. 启动服务器
    TcpServer server(cfg.server.listen_ip, cfg.server.listen_port, handler);
    if (push_stream) {
        push_stream->SetDeliveryCallback([&server, &cfg](const chat::RemotePushEvent& event) {
            return server.deliverRemotePush(event, std::chrono::milliseconds(cfg.timeout.remote_push_ms));
        });
        push_stream->SetMarkDeliveredCallback([&message_repo](chat::UserId user_id, const std::string& message_id) {
            return message_repo.markDelivered(user_id, {message_id}).status == chat::RepositoryStatus::kOk;
        });
        push_stream->Start();
    }
    const int result = RunMain([&]() { return server.start(); });
    if (push_stream) {
        push_stream->Stop();
    }
    return result;
}
