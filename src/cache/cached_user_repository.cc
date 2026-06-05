#include "cache/cached_user_repository.h"

#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

namespace chat {
namespace {

constexpr char kNotFoundValue[] = "__not_found__";

std::string EncodeUser(const UserRecord &user) {
    return nlohmann::json{{"id", user.id},
                          {"username", user.username},
                          {"password_hash", user.password_hash},
                          {"nickname", user.nickname},
                          {"status", user.status},
                          {"created_at", user.created_at},
                          {"updated_at", user.updated_at}}
        .dump();
}

std::optional<UserRecord> DecodeUser(const std::string &value) {
    try {
        const nlohmann::json json = nlohmann::json::parse(value);
        UserRecord user;
        user.id = json.at("id").get<UserId>();
        user.username = json.at("username").get<std::string>();
        user.password_hash = json.at("password_hash").get<std::string>();
        user.nickname = json.at("nickname").get<std::string>();
        user.status = json.at("status").get<int>();
        user.created_at = json.at("created_at").get<std::string>();
        user.updated_at = json.at("updated_at").get<std::string>();
        if (user.id <= 0 || user.username.empty()) {
            return std::nullopt;
        }
        return user;
    } catch (const nlohmann::json::exception &) {
        return std::nullopt;
    }
}

}  // namespace

CachedUserRepository::CachedUserRepository(IUserRepository *source, IRedisClient *redis, RedisConfig config)
    : source_(source), redis_(redis), config_(std::move(config)) {}

FindUserResult CachedUserRepository::findByUsername(const std::string &username) {
    const std::string key = UsernameKey(username);
    if (redis_ != nullptr) {
        const RedisCommandResult cached = redis_->Command({"GET", key});
        if (cached.ok() && cached.reply.type == RedisReplyType::kString) {
            if (cached.reply.string_value == kNotFoundValue) {
                return {.status = RepositoryStatus::kNotFound};
            }
            try {
                const UserId user_id = std::stoll(cached.reply.string_value);
                const FindUserResult by_id = findById(user_id);
                if (by_id.status == RepositoryStatus::kOk) {
                    return by_id;
                }
            } catch (const std::exception &) {
                redis_->Command({"DEL", key});
            }
        }
    }

    const FindUserResult result = source_->findByUsername(username);
    if (result.status == RepositoryStatus::kOk && result.user) {
        CacheUser(*result.user);
    } else if (result.status == RepositoryStatus::kNotFound) {
        CacheNotFound(key);
    }
    return result;
}

FindUserResult CachedUserRepository::findById(UserId user_id) {
    const std::string key = UserKey(user_id);
    if (const auto cached = ReadUser(key)) {
        return *cached;
    }
    const FindUserResult result = source_->findById(user_id);
    if (result.status == RepositoryStatus::kOk && result.user) {
        CacheUser(*result.user);
    } else if (result.status == RepositoryStatus::kNotFound) {
        CacheNotFound(key);
    }
    return result;
}

CreateUserResult CachedUserRepository::createUser(const std::string &username, const std::string &password_hash,
                                                  const std::string &nickname) {
    const CreateUserResult result = source_->createUser(username, password_hash, nickname);
    if (redis_ != nullptr && result.status == RepositoryStatus::kOk) {
        // 新建后先失效，后续读取统一从 MySQL 装载完整记录。
        redis_->Command({"DEL", UsernameKey(username), UserKey(result.user_id)});
    }
    return result;
}

std::string CachedUserRepository::UserKey(UserId user_id) const {
    return config_.key_prefix + ":user:id:" + std::to_string(user_id);
}

std::string CachedUserRepository::UsernameKey(const std::string &username) const {
    return config_.key_prefix + ":user:name:" + username;
}

void CachedUserRepository::CacheUser(const UserRecord &user) {
    if (redis_ == nullptr) {
        return;
    }
    redis_->Command({"SETEX", UserKey(user.id), std::to_string(config_.user_cache_ttl_seconds), EncodeUser(user)});
    redis_->Command(
        {"SETEX", UsernameKey(user.username), std::to_string(config_.user_cache_ttl_seconds), std::to_string(user.id)});
}

void CachedUserRepository::CacheNotFound(const std::string &key) {
    if (redis_ != nullptr) {
        redis_->Command({"SETEX", key, std::to_string(config_.user_not_found_ttl_seconds), kNotFoundValue});
    }
}

std::optional<FindUserResult> CachedUserRepository::ReadUser(const std::string &key) {
    if (redis_ == nullptr) {
        return std::nullopt;
    }
    const RedisCommandResult cached = redis_->Command({"GET", key});
    if (!cached.ok() || cached.reply.type != RedisReplyType::kString) {
        return std::nullopt;
    }
    if (cached.reply.string_value == kNotFoundValue) {
        return FindUserResult{.status = RepositoryStatus::kNotFound};
    }
    const auto user = DecodeUser(cached.reply.string_value);
    if (!user) {
        redis_->Command({"DEL", key});
        return std::nullopt;
    }
    return FindUserResult{.status = RepositoryStatus::kOk, .user = user};
}

}  // namespace chat
