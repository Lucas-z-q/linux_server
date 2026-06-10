#include "redis/redis_connection.h"

#include <hiredis.h>

#include <cerrno>
#include <chrono>
#include <utility>

namespace chat {
namespace {

RedisCommandResult MakeError(RedisError error, const std::string &message) {
    return {.success = false, .error = error, .message = message};
}

RedisReply ConvertReply(const redisReply *reply) {
    RedisReply result;
    if (reply == nullptr) {
        return result;
    }
    switch (reply->type) {
        case REDIS_REPLY_STRING:
            result.type = RedisReplyType::kString;
            result.string_value.assign(reply->str, reply->len);
            break;
        case REDIS_REPLY_STATUS:
            result.type = RedisReplyType::kStatus;
            result.string_value.assign(reply->str, reply->len);
            break;
        case REDIS_REPLY_INTEGER:
            result.type = RedisReplyType::kInteger;
            result.integer_value = reply->integer;
            break;
        case REDIS_REPLY_ARRAY:
            result.type = RedisReplyType::kArray;
            result.elements.reserve(reply->elements);
            for (std::size_t i = 0; i < reply->elements; ++i) {
                result.elements.push_back(ConvertReply(reply->element[i]));
            }
            break;
        case REDIS_REPLY_NIL:
        default:
            result.type = RedisReplyType::kNil;
            break;
    }
    return result;
}

timeval ToTimeval(std::uint32_t milliseconds) {
    timeval timeout{};
    timeout.tv_sec = milliseconds / 1000;
    timeout.tv_usec = static_cast<suseconds_t>((milliseconds % 1000) * 1000);
    return timeout;
}

}  // namespace

RedisConnection::RedisConnection(const RedisConfig &config) : config_(config) {}

RedisConnection::~RedisConnection() noexcept { Close(); }

RedisConnectionResult RedisConnection::Connect() {
    Close();
    const RedisConfigResult validation = ValidateRedisConfig(config_);
    if (!validation.ok()) {
        return MakeError(RedisError::kInvalidConfig, validation.message);
    }

    redisOptions options{};
    REDIS_OPTIONS_SET_TCP(&options, config_.host.c_str(), config_.port);
    const timeval connect_timeout = ToTimeval(config_.connect_timeout_ms);
    const timeval command_timeout = ToTimeval(config_.command_timeout_ms);
    options.connect_timeout = &connect_timeout;
    options.command_timeout = &command_timeout;
    context_ = redisConnectWithOptions(&options);
    if (context_ == nullptr || context_->err != 0) {
        const std::string message = context_ == nullptr ? "cannot allocate redis context" : context_->errstr;
        Close();
        return MakeError(errno == ETIMEDOUT ? RedisError::kTimeout : RedisError::kConnectionUnavailable, message);
    }

    if (!config_.password.empty()) {
        RedisCommandResult auth = RunSetupCommand({"AUTH", config_.password}, RedisError::kAuthFailed);
        if (!auth.ok()) {
            return auth;
        }
    }
    if (config_.database != 0) {
        RedisCommandResult select =
            RunSetupCommand({"SELECT", std::to_string(config_.database)}, RedisError::kSelectFailed);
        if (!select.ok()) {
            return select;
        }
    }
    return {.success = true, .error = RedisError::kNone, .message = "connect success"};
}

RedisCommandResult RedisConnection::Ping() { return Execute({"PING"}); }

RedisCommandResult RedisConnection::Execute(const std::vector<std::string> &args) {
    if (context_ == nullptr || context_->err != 0) {
        return MakeError(RedisError::kConnectionUnavailable, "redis connection is unavailable");
    }
    if (args.empty()) {
        return MakeError(RedisError::kCommandFailed, "redis command is empty");
    }

    std::vector<const char *> argv;
    std::vector<std::size_t> lengths;
    argv.reserve(args.size());
    lengths.reserve(args.size());
    for (const std::string &arg : args) {
        argv.push_back(arg.data());
        lengths.push_back(arg.size());
    }

    std::unique_ptr<redisReply, decltype(&freeReplyObject)> reply(
        static_cast<redisReply *>(
            redisCommandArgv(context_, static_cast<int>(argv.size()), argv.data(), lengths.data())),
        &freeReplyObject);
    if (!reply) {
        const RedisError error = errno == ETIMEDOUT ? RedisError::kTimeout : RedisError::kConnectionUnavailable;
        return MakeError(error, context_->errstr);
    }
    if (reply->type == REDIS_REPLY_ERROR) {
        return MakeError(RedisError::kCommandFailed, std::string(reply->str, reply->len));
    }
    if (reply->type == REDIS_REPLY_NIL) {
        RedisCommandResult result = MakeError(RedisError::kNotFound, "redis key not found");
        result.reply = ConvertReply(reply.get());
        return result;
    }
    return {.success = true, .error = RedisError::kNone, .message = "ok", .reply = ConvertReply(reply.get())};
}

void RedisConnection::Close() noexcept {
    if (context_ != nullptr) {
        redisFree(context_);
        context_ = nullptr;
    }
}

bool RedisConnection::IsConnected() const noexcept { return context_ != nullptr && context_->err == 0; }

RedisCommandResult RedisConnection::RunSetupCommand(const std::vector<std::string> &args, RedisError setup_error) {
    RedisCommandResult result = Execute(args);
    if (!result.ok()) {
        const std::string message = result.message;
        Close();
        return MakeError(setup_error, message);
    }
    return result;
}

std::unique_ptr<RedisConnection> DefaultRedisConnectionFactory::Create(const RedisConfig &config) {
    return std::make_unique<RedisConnection>(config);
}

}  // namespace chat
