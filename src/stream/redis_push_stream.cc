#include "stream/redis_push_stream.h"

#include <chrono>
#include <cstdlib>
#include <utility>

namespace chat {
namespace {

constexpr char kPushGroup[] = "chat-push";
constexpr char kRetryGroup[] = "chat-delivery-retry";
constexpr std::uint32_t kDeliveryDedupTtlSeconds = 86400;
constexpr std::uint32_t kClaimIdleMilliseconds = 30000;
constexpr std::uint32_t kReadBlockMilliseconds = 200;
constexpr std::uint32_t kBatchSize = 20;

std::optional<long long> ParseInteger(const std::string &value) {
    char *end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return parsed;
}

std::unordered_map<std::string, std::string> ParseFields(const RedisReply &reply) {
    std::unordered_map<std::string, std::string> fields;
    if (reply.type != RedisReplyType::kArray || reply.elements.size() % 2 != 0) {
        return fields;
    }
    for (std::size_t i = 0; i < reply.elements.size(); i += 2) {
        if (reply.elements[i].type != RedisReplyType::kString ||
            reply.elements[i + 1].type != RedisReplyType::kString) {
            fields.clear();
            return fields;
        }
        fields[reply.elements[i].string_value] = reply.elements[i + 1].string_value;
    }
    return fields;
}

}  // namespace

RedisPushStream::RedisPushStream(IRedisClient *redis, RedisConfig config) : redis_(redis), config_(std::move(config)) {}

RedisPushStream::~RedisPushStream() { Stop(); }

bool RedisPushStream::Initialize() {
    return redis_ != nullptr && EnsureGroup(PushStreamKey(config_.server_id), kPushGroup) &&
           EnsureGroup(RetryStreamKey(config_.server_id), kRetryGroup);
}

bool RedisPushStream::Publish(const std::string &target_server_id, const RemotePushEvent &event) {
    if (redis_ == nullptr || target_server_id.empty() || target_server_id.find(':') != std::string::npos ||
        !DecodePushEvent({.fields = {{"event_id", event.event_id},
                                     {"message_id", event.message_id},
                                     {"from_user_id", std::to_string(event.from_user_id)},
                                     {"to_user_id", std::to_string(event.to_user_id)},
                                     {"connection_id", std::to_string(event.target_connection_id)},
                                     {"payload", event.payload}}})) {
        return false;
    }
    const RedisCommandResult result = redis_->Command(
        {"XADD", PushStreamKey(target_server_id), "*", "event_id", event.event_id, "message_id", event.message_id,
         "from_user_id", std::to_string(event.from_user_id), "to_user_id", std::to_string(event.to_user_id),
         "connection_id", std::to_string(event.target_connection_id), "payload", event.payload});
    return result.ok() && result.reply.type == RedisReplyType::kString;
}

void RedisPushStream::SetDeliveryCallback(DeliveryCallback callback) { delivery_callback_ = std::move(callback); }

void RedisPushStream::SetMarkDeliveredCallback(MarkDeliveredCallback callback) {
    mark_delivered_callback_ = std::move(callback);
}

void RedisPushStream::Start() {
    if (running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&RedisPushStream::Run, this);
}

void RedisPushStream::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool RedisPushStream::PollOnce() {
    if (redis_ == nullptr) {
        return false;
    }

    const std::string push_stream = PushStreamKey(config_.server_id);
    ReadBatch claimed_push = ClaimPending(push_stream, kPushGroup);
    ReadBatch new_push = ReadNew(push_stream, kPushGroup, true);
    claimed_push.entries.insert(claimed_push.entries.end(), new_push.entries.begin(), new_push.entries.end());
    std::vector<StreamEntry> &push_entries = claimed_push.entries;
    for (const StreamEntry &entry : push_entries) {
        ProcessPushEntry(entry);
    }

    const std::string retry_stream = RetryStreamKey(config_.server_id);
    ReadBatch claimed_retry = ClaimPending(retry_stream, kRetryGroup);
    ReadBatch new_retry = ReadNew(retry_stream, kRetryGroup, false);
    claimed_retry.entries.insert(claimed_retry.entries.end(), new_retry.entries.begin(), new_retry.entries.end());
    std::vector<StreamEntry> &retry_entries = claimed_retry.entries;
    for (const StreamEntry &entry : retry_entries) {
        ProcessRetryEntry(entry);
    }
    return claimed_push.success && new_push.success && claimed_retry.success && new_retry.success;
}

std::string RedisPushStream::PushStreamKey(const std::string &server_id) const {
    return config_.key_prefix + ":push:stream:" + server_id;
}

std::string RedisPushStream::RetryStreamKey(const std::string &server_id) const {
    return config_.key_prefix + ":delivery:retry:" + server_id;
}

std::string RedisPushStream::DeadLetterStreamKey(const std::string &server_id) const {
    return config_.key_prefix + ":push:dlq:" + server_id;
}

bool RedisPushStream::EnsureGroup(const std::string &stream, const std::string &group) {
    const RedisCommandResult result = redis_->Command({"XGROUP", "CREATE", stream, group, "0", "MKSTREAM"});
    return result.ok() || result.message.find("BUSYGROUP") != std::string::npos;
}

RedisPushStream::ReadBatch RedisPushStream::ReadNew(const std::string &stream, const std::string &group, bool block) {
    std::vector<std::string> command = {"XREADGROUP",      "GROUP", group,
                                        config_.server_id, "COUNT", std::to_string(kBatchSize)};
    if (block) {
        command.insert(command.end(), {"BLOCK", std::to_string(kReadBlockMilliseconds)});
    }
    command.insert(command.end(), {"STREAMS", stream, ">"});
    const RedisCommandResult result = redis_->Command(command);
    if (result.ok()) {
        return {.success = true, .entries = ParseReadReply(result.reply)};
    }
    return {.success = result.error == RedisError::kNotFound};
}

RedisPushStream::ReadBatch RedisPushStream::ClaimPending(const std::string &stream, const std::string &group) {
    const RedisCommandResult result =
        redis_->Command({"XAUTOCLAIM", stream, group, config_.server_id, std::to_string(kClaimIdleMilliseconds), "0-0",
                         "COUNT", std::to_string(kBatchSize)});
    if (result.ok()) {
        return {.success = true, .entries = ParseClaimReply(result.reply)};
    }
    return {};
}

std::vector<RedisPushStream::StreamEntry> RedisPushStream::ParseReadReply(const RedisReply &reply) const {
    if (reply.type != RedisReplyType::kArray || reply.elements.empty()) {
        return {};
    }
    std::vector<StreamEntry> entries;
    for (const RedisReply &stream_reply : reply.elements) {
        if (stream_reply.type != RedisReplyType::kArray || stream_reply.elements.size() != 2 ||
            stream_reply.elements[1].type != RedisReplyType::kArray) {
            continue;
        }
        for (const RedisReply &entry_reply : stream_reply.elements[1].elements) {
            if (entry_reply.type != RedisReplyType::kArray || entry_reply.elements.size() != 2 ||
                entry_reply.elements[0].type != RedisReplyType::kString) {
                continue;
            }
            entries.push_back(
                {.stream_id = entry_reply.elements[0].string_value, .fields = ParseFields(entry_reply.elements[1])});
        }
    }
    return entries;
}

std::vector<RedisPushStream::StreamEntry> RedisPushStream::ParseClaimReply(const RedisReply &reply) const {
    if (reply.type != RedisReplyType::kArray || reply.elements.size() < 2 ||
        reply.elements[1].type != RedisReplyType::kArray) {
        return {};
    }
    RedisReply wrapped;
    wrapped.type = RedisReplyType::kArray;
    RedisReply stream;
    stream.type = RedisReplyType::kArray;
    stream.elements.resize(2);
    stream.elements[1] = reply.elements[1];
    wrapped.elements.push_back(std::move(stream));
    return ParseReadReply(wrapped);
}

bool RedisPushStream::ProcessPushEntry(const StreamEntry &entry) {
    const auto event = DecodePushEvent(entry);
    const std::string stream = PushStreamKey(config_.server_id);
    if (!event) {
        return MoveToDeadLetter(stream, entry, "invalid push event") && Ack(stream, kPushGroup, entry.stream_id);
    }
    if (!delivery_callback_) {
        return false;
    }

    // SET NX 在消费者崩溃恢复时抑制重复正文投递；失败时保留 pending。
    const std::string dedup_key = DeliveryDedupKey(event->message_id);
    const RedisCommandResult acquired =
        redis_->Command({"SET", dedup_key, "1", "NX", "EX", std::to_string(kDeliveryDedupTtlSeconds)});
    if (!acquired.ok()) {
        if (acquired.error == RedisError::kNotFound) {
            return Ack(stream, kPushGroup, entry.stream_id);
        }
        return false;
    }

    const RemoteDeliveryOutcome outcome = delivery_callback_(*event);
    if (outcome == RemoteDeliveryOutcome::kRetry) {
        redis_->Command({"DEL", dedup_key});
        return false;
    }
    return Ack(stream, kPushGroup, entry.stream_id);
}

bool RedisPushStream::ProcessRetryEntry(const StreamEntry &entry) {
    const auto user = entry.fields.find("to_user_id");
    const auto message = entry.fields.find("message_id");
    const std::string stream = RetryStreamKey(config_.server_id);
    if (user == entry.fields.end() || message == entry.fields.end()) {
        return MoveToDeadLetter(stream, entry, "invalid delivery retry") && Ack(stream, kRetryGroup, entry.stream_id);
    }
    const auto user_id = ParseInteger(user->second);
    if (!user_id || *user_id <= 0 || message->second.empty()) {
        return MoveToDeadLetter(stream, entry, "invalid delivery retry fields") &&
               Ack(stream, kRetryGroup, entry.stream_id);
    }
    if (mark_delivered_callback_ && mark_delivered_callback_(*user_id, message->second)) {
        return Ack(stream, kRetryGroup, entry.stream_id);
    }
    return false;
}

bool RedisPushStream::Ack(const std::string &stream, const std::string &group, const std::string &stream_id) {
    const RedisCommandResult result = redis_->Command({"XACK", stream, group, stream_id});
    return result.ok() && result.reply.type == RedisReplyType::kInteger && result.reply.integer_value == 1;
}

bool RedisPushStream::MoveToDeadLetter(const std::string &source_stream, const StreamEntry &entry,
                                       const std::string &reason) {
    return redis_
        ->Command({"XADD", DeadLetterStreamKey(config_.server_id), "*", "source_stream", source_stream, "source_id",
                   entry.stream_id, "reason", reason})
        .ok();
}

bool RedisPushStream::PublishDeliveryRetry(UserId user_id, const std::string &message_id) {
    return redis_
        ->Command({"XADD", RetryStreamKey(config_.server_id), "*", "to_user_id", std::to_string(user_id), "message_id",
                   message_id})
        .ok();
}

std::optional<RemotePushEvent> RedisPushStream::DecodePushEvent(const StreamEntry &entry) const {
    const auto event_id = entry.fields.find("event_id");
    const auto message_id = entry.fields.find("message_id");
    const auto from_user_id = entry.fields.find("from_user_id");
    const auto to_user_id = entry.fields.find("to_user_id");
    const auto connection_id = entry.fields.find("connection_id");
    const auto payload = entry.fields.find("payload");
    if (event_id == entry.fields.end() || message_id == entry.fields.end() || from_user_id == entry.fields.end() ||
        to_user_id == entry.fields.end() || connection_id == entry.fields.end() || payload == entry.fields.end()) {
        return std::nullopt;
    }
    const auto from = ParseInteger(from_user_id->second);
    const auto to = ParseInteger(to_user_id->second);
    const auto connection = ParseInteger(connection_id->second);
    if (event_id->second.empty() || message_id->second.empty() || !from || *from <= 0 || !to || *to <= 0 ||
        !connection || *connection <= 0 || payload->second.empty()) {
        return std::nullopt;
    }
    return RemotePushEvent{event_id->second, message_id->second, *from, *to, static_cast<ConnectionId>(*connection),
                           payload->second};
}

std::string RedisPushStream::DeliveryDedupKey(const std::string &message_id) const {
    return config_.key_prefix + ":push:dedup:" + config_.server_id + ":" + message_id;
}

void RedisPushStream::Run() {
    while (running_.load()) {
        if (!PollOnce()) {
            EnsureGroup(PushStreamKey(config_.server_id), kPushGroup);
            EnsureGroup(RetryStreamKey(config_.server_id), kRetryGroup);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

}  // namespace chat
