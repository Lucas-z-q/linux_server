#include "cache/cached_user_repository.h"

#include <exception>
#include <utility>

namespace chat {
namespace {

constexpr char kNotFoundValue[] = "__not_found__";

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
    if (result.status == RepositoryStatus::kNotFound) {
        CacheNotFound(key);
    }
    return result;
}

FindUserResult CachedUserRepository::findById(UserId user_id) {
    const std::string key = UserKey(user_id);
    if (redis_ != nullptr) {
        const RedisCommandResult cached = redis_->Command({"GET", key});
        if (cached.ok() && cached.reply.type == RedisReplyType::kString &&
            cached.reply.string_value == kNotFoundValue) {
            return {.status = RepositoryStatus::kNotFound};
        }
    }
    const FindUserResult result = source_->findById(user_id);
    if (result.status == RepositoryStatus::kNotFound) {
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

RepositoryStatus CachedUserRepository::updatePasswordHash(UserId user_id, const std::string &password_hash) {
    const RepositoryStatus status = source_->updatePasswordHash(user_id, password_hash);
    if (redis_ != nullptr && status == RepositoryStatus::kOk) {
        redis_->Command({"DEL", UserKey(user_id)});
    }
    return status;
}

void CachedUserRepository::CacheNotFound(const std::string &key) {
    if (redis_ != nullptr) {
        redis_->Command({"SETEX", key, std::to_string(config_.user_not_found_ttl_seconds), kNotFoundValue});
    }
}

}  // namespace chat
