#include "server/redis_session_store.h"

#include <cstdlib>
#include <utility>

#include "common/validator.h"

namespace chat {
namespace {

// 原子替换两个 presence 索引，并清理被挤下线的旧索引。
constexpr char kBindScript[] = R"(
local old_server = redis.call('HGET', KEYS[2], 'server_id')
local old_conn = redis.call('HGET', KEYS[2], 'connection_id')
local old_presence_token = redis.call('HGET', KEYS[4], 'token')
if old_presence_token and old_presence_token ~= ARGV[8] then
  redis.call('DEL', ARGV[1] .. ':session:token:' .. old_presence_token)
end
if old_server and old_conn then
  redis.call('DEL', ARGV[1] .. ':presence:conn:' .. old_server .. ':' .. old_conn)
end
local old_user = redis.call('HGET', KEYS[3], 'user_id')
local old_token = redis.call('HGET', KEYS[3], 'token')
if old_user and old_token then
  local old_user_key = ARGV[1] .. ':presence:user:' .. old_user
  if redis.call('HGET', old_user_key, 'server_id') == ARGV[2]
     and redis.call('HGET', old_user_key, 'connection_id') == ARGV[3]
     and redis.call('HGET', old_user_key, 'token') == old_token then
    redis.call('DEL', old_user_key)
  end
end
redis.call('HSET', KEYS[1], 'user_id', ARGV[4], 'username', ARGV[5], 'issued_at', ARGV[6])
redis.call('EXPIRE', KEYS[1], ARGV[7])
redis.call('HSET', KEYS[2], 'server_id', ARGV[2], 'connection_id', ARGV[3], 'token', ARGV[8])
redis.call('EXPIRE', KEYS[2], ARGV[9])
redis.call('HSET', KEYS[3], 'user_id', ARGV[4], 'token', ARGV[8])
redis.call('EXPIRE', KEYS[3], ARGV[9])
redis.call('HSET', KEYS[4], 'token', ARGV[8])
redis.call('EXPIRE', KEYS[4], ARGV[7])
return 1
)";

// 旧连接不能给后来登录的新 presence 续期。
constexpr char kRefreshScript[] = R"(
if redis.call('HGET', KEYS[1], 'server_id') ~= ARGV[1]
   or redis.call('HGET', KEYS[1], 'connection_id') ~= ARGV[2]
   or redis.call('HGET', KEYS[1], 'token') ~= ARGV[3]
   or redis.call('HGET', KEYS[2], 'user_id') ~= ARGV[4]
   or redis.call('HGET', KEYS[2], 'token') ~= ARGV[3] then
  return 0
end
redis.call('EXPIRE', KEYS[1], ARGV[5])
redis.call('EXPIRE', KEYS[2], ARGV[5])
return 1
)";

// 条件删除避免旧连接关闭时误删新登录的 presence。
constexpr char kClearScript[] = R"(
redis.call('DEL', KEYS[2])
if redis.call('HGET', KEYS[1], 'server_id') == ARGV[1]
   and redis.call('HGET', KEYS[1], 'connection_id') == ARGV[2]
   and redis.call('HGET', KEYS[1], 'token') == ARGV[3] then
  redis.call('DEL', KEYS[1])
end
return 1
)";

constexpr char kRevokeScript[] = R"(
redis.call('DEL', KEYS[3])
if redis.call('HGET', KEYS[4], 'token') == ARGV[3] then
  redis.call('DEL', KEYS[4])
end
redis.call('DEL', KEYS[2])
if redis.call('HGET', KEYS[1], 'server_id') == ARGV[1]
   and redis.call('HGET', KEYS[1], 'connection_id') == ARGV[2]
   and redis.call('HGET', KEYS[1], 'token') == ARGV[3] then
  redis.call('DEL', KEYS[1])
end
return 1
)";

std::optional<long long> ParseInteger(const std::string &value) {
    char *end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

bool IsOne(const RedisCommandResult &result) {
    return result.ok() && result.reply.type == RedisReplyType::kInteger && result.reply.integer_value == 1;
}

}  // namespace

RedisSessionStore::RedisSessionStore(IRedisClient *client, RedisConfig config)
    : client_(client), config_(std::move(config)) {}

bool RedisSessionStore::Bind(ConnectionId connection_id, const ConnectionSession &session, Timestamp issued_at) {
    if (client_ == nullptr || connection_id == 0 || issued_at < 0 || !IsValidSession(session)) {
        return false;
    }
    return IsOne(client_->Command({"EVAL", kBindScript, "4", TokenKey(session.token), UserPresenceKey(session.user_id),
                                   ConnectionPresenceKey(config_.server_id, connection_id),
                                   UserSessionKey(session.user_id), config_.key_prefix, config_.server_id,
                                   std::to_string(connection_id), std::to_string(session.user_id), session.username,
                                   std::to_string(issued_at), std::to_string(config_.session_ttl_seconds),
                                   session.token, std::to_string(config_.presence_ttl_seconds)}));
}

bool RedisSessionStore::Refresh(ConnectionId connection_id, const ConnectionSession &session) {
    if (client_ == nullptr || connection_id == 0 || !IsValidSession(session)) {
        return false;
    }
    return IsOne(client_->Command({"EVAL", kRefreshScript, "2", UserPresenceKey(session.user_id),
                                   ConnectionPresenceKey(config_.server_id, connection_id), config_.server_id,
                                   std::to_string(connection_id), session.token, std::to_string(session.user_id),
                                   std::to_string(config_.presence_ttl_seconds)}));
}

bool RedisSessionStore::ClearPresence(ConnectionId connection_id, const ConnectionSession &session) {
    if (client_ == nullptr || connection_id == 0 || !IsValidSession(session)) {
        return false;
    }
    return client_
        ->Command({"EVAL", kClearScript, "2", UserPresenceKey(session.user_id),
                   ConnectionPresenceKey(config_.server_id, connection_id), config_.server_id,
                   std::to_string(connection_id), session.token})
        .ok();
}

bool RedisSessionStore::RevokeSession(ConnectionId connection_id, const ConnectionSession &session) {
    if (client_ == nullptr || connection_id == 0 || !IsValidSession(session)) {
        return false;
    }
    return IsOne(client_->Command({"EVAL", kRevokeScript, "4", UserPresenceKey(session.user_id),
                                   ConnectionPresenceKey(config_.server_id, connection_id), TokenKey(session.token),
                                   UserSessionKey(session.user_id), config_.server_id, std::to_string(connection_id),
                                   session.token}));
}

bool RedisSessionStore::RevokeToken(const std::string &token) {
    return client_ != nullptr && Validator::Token(token).ok() && client_->Command({"DEL", TokenKey(token)}).ok();
}

std::optional<StoredSessionToken> RedisSessionStore::GetToken(const std::string &token) {
    if (!Validator::Token(token).ok()) {
        return std::nullopt;
    }
    const auto values = ReadHash(TokenKey(token), {"user_id", "username", "issued_at"});
    if (!values) {
        return std::nullopt;
    }
    const auto user_id = ParseInteger((*values)[0]);
    const auto issued_at = ParseInteger((*values)[2]);
    if (!user_id || *user_id <= 0 || (*values)[1].empty() || !issued_at || *issued_at < 0) {
        client_->Command({"DEL", TokenKey(token)});
        return std::nullopt;
    }
    return StoredSessionToken{*user_id, (*values)[1], *issued_at};
}

std::optional<StoredUserPresence> RedisSessionStore::GetPresence(UserId user_id) {
    if (user_id <= 0) {
        return std::nullopt;
    }
    const auto values = ReadHash(UserPresenceKey(user_id), {"server_id", "connection_id", "token"});
    if (!values) {
        return std::nullopt;
    }
    const auto connection_id = ParseInteger((*values)[1]);
    if ((*values)[0].empty() || !connection_id || *connection_id <= 0 || !Validator::Token((*values)[2]).ok()) {
        client_->Command({"DEL", UserPresenceKey(user_id)});
        return std::nullopt;
    }
    return StoredUserPresence{(*values)[0], static_cast<ConnectionId>(*connection_id), (*values)[2]};
}

std::string RedisSessionStore::TokenKey(const std::string &token) const {
    return config_.key_prefix + ":session:token:" + token;
}

std::string RedisSessionStore::UserPresenceKey(UserId user_id) const {
    return config_.key_prefix + ":presence:user:" + std::to_string(user_id);
}

std::string RedisSessionStore::UserSessionKey(UserId user_id) const {
    return config_.key_prefix + ":session:user:" + std::to_string(user_id);
}

std::string RedisSessionStore::ConnectionPresenceKey(const std::string &server_id, ConnectionId connection_id) const {
    return config_.key_prefix + ":presence:conn:" + server_id + ":" + std::to_string(connection_id);
}

bool RedisSessionStore::IsValidSession(const ConnectionSession &session) const {
    return session.authenticated && session.user_id > 0 && !session.username.empty() &&
           Validator::Token(session.token).ok();
}

std::optional<std::vector<std::string>> RedisSessionStore::ReadHash(const std::string &key,
                                                                    const std::vector<std::string> &fields) {
    if (client_ == nullptr || key.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> command = {"HMGET", key};
    command.insert(command.end(), fields.begin(), fields.end());
    const RedisCommandResult result = client_->Command(command);
    if (!result.ok() || result.reply.type != RedisReplyType::kArray || result.reply.elements.size() != fields.size()) {
        return std::nullopt;
    }
    std::vector<std::string> values;
    for (const RedisReply &element : result.reply.elements) {
        if (element.type != RedisReplyType::kString) {
            return std::nullopt;
        }
        values.push_back(element.string_value);
    }
    return values;
}

}  // namespace chat
