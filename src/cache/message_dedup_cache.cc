#include "cache/message_dedup_cache.h"

#include <utility>

namespace chat {

MessageDedupCache::MessageDedupCache(IRedisClient *redis, RedisConfig config)
    : redis_(redis), config_(std::move(config)) {}

std::optional<std::string> MessageDedupCache::Lookup(UserId from_user_id, const std::string &client_msg_id) {
    if (redis_ == nullptr) {
        return std::nullopt;
    }
    const RedisCommandResult result = redis_->Command({"GET", Key(from_user_id, client_msg_id)});
    if (!result.ok() || result.reply.type != RedisReplyType::kString || result.reply.string_value.empty()) {
        return std::nullopt;
    }
    return result.reply.string_value;
}

void MessageDedupCache::Save(UserId from_user_id, const std::string &client_msg_id, const std::string &message_id) {
    if (redis_ != nullptr && !message_id.empty()) {
        redis_->Command(
            {"SETEX", Key(from_user_id, client_msg_id), std::to_string(config_.message_dedup_ttl_seconds), message_id});
    }
}

void MessageDedupCache::Remove(UserId from_user_id, const std::string &client_msg_id) {
    if (redis_ != nullptr) {
        redis_->Command({"DEL", Key(from_user_id, client_msg_id)});
    }
}

std::string MessageDedupCache::Key(UserId from_user_id, const std::string &client_msg_id) const {
    return config_.key_prefix + ":dedup:msg:" + std::to_string(from_user_id) + ":" + client_msg_id;
}

}  // namespace chat
