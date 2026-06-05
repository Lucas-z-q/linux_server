#ifndef LINUX_SERVER_INCLUDE_REDIS_REDIS_CONNECTION_H_
#define LINUX_SERVER_INCLUDE_REDIS_REDIS_CONNECTION_H_

#include <memory>
#include <string>
#include <vector>

#include "config/redis_config.h"

struct redisContext;

namespace chat {

enum class RedisError {
    kNone = 0,
    kInvalidConfig,
    kConnectionUnavailable,
    kAuthFailed,
    kSelectFailed,
    kTimeout,
    kCommandFailed,
    kNotFound,
};

enum class RedisReplyType {
    kNil,
    kString,
    kStatus,
    kInteger,
    kArray,
};

struct RedisReply {
    RedisReplyType type = RedisReplyType::kNil;
    std::string string_value;
    long long integer_value = 0;
    std::vector<RedisReply> elements;
};

struct RedisCommandResult {
    bool success = false;
    RedisError error = RedisError::kNone;
    std::string message;
    RedisReply reply;

    bool ok() const noexcept { return success; }
};

using RedisConnectionResult = RedisCommandResult;

// 包装一个 hiredis 同步连接。对象不跨线程共享，由连接池独占借出。
class RedisConnection {
   public:
    explicit RedisConnection(const RedisConfig &config);
    virtual ~RedisConnection() noexcept;

    RedisConnection(const RedisConnection &) = delete;
    RedisConnection &operator=(const RedisConnection &) = delete;

    virtual RedisConnectionResult Connect();
    virtual RedisCommandResult Ping();
    virtual RedisCommandResult Execute(const std::vector<std::string> &args);
    virtual void Close() noexcept;
    virtual bool IsConnected() const noexcept;

   private:
    RedisCommandResult RunSetupCommand(const std::vector<std::string> &args, RedisError setup_error);

    RedisConfig config_;
    redisContext *context_ = nullptr;
};

class IRedisConnectionFactory {
   public:
    virtual ~IRedisConnectionFactory() = default;
    virtual std::unique_ptr<RedisConnection> Create(const RedisConfig &config) = 0;
};

class DefaultRedisConnectionFactory : public IRedisConnectionFactory {
   public:
    std::unique_ptr<RedisConnection> Create(const RedisConfig &config) override;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_REDIS_REDIS_CONNECTION_H_
