#include "service/chat_service.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace chat {

namespace {

constexpr int32_t kMaxPullOfflineLimit = 100;

ErrorCode MapRepositoryError(RepositoryStatus status) {
    switch (status) {
        case RepositoryStatus::kInsertFailed:
        case RepositoryStatus::kDuplicate:
            return ErrorCode::DB_INSERT_FAILED;
        case RepositoryStatus::kQueryFailed:
        case RepositoryStatus::kConnectionUnavailable:
        case RepositoryStatus::kBorrowTimeout:
        case RepositoryStatus::kNotFound:
            return ErrorCode::DB_QUERY_FAILED;
        default:
            return ErrorCode::INTERNAL_ERROR;
    }
}

std::string BuildConversationId(UserId user_a, UserId user_b) {
    const UserId min_id = std::min(user_a, user_b);
    const UserId max_id = std::max(user_a, user_b);
    return "conv_" + std::to_string(min_id) + "_" + std::to_string(max_id);
}

std::string BuildMessageId(Timestamp now, UserId from_user_id) {
    static std::atomic<uint64_t> msg_seq{0};
    static thread_local std::mt19937_64 rng(std::random_device{}());

    const uint64_t random_part = rng();
    const uint64_t mixed_part = (static_cast<uint64_t>(now) << 32) ^ (static_cast<uint64_t>(getpid()) << 16) ^
                                static_cast<uint64_t>(from_user_id) ^ (++msg_seq);

    std::ostringstream oss;
    oss << "msg_" << std::hex << std::setw(16) << std::setfill('0') << mixed_part << std::setw(16) << std::setfill('0')
        << random_part;
    return oss.str();
}

bool IsSameIdempotentMessage(const MessageRecord& expected, const MessageRecord& actual) {
    return expected.conversation_id == actual.conversation_id && expected.client_msg_id == actual.client_msg_id &&
           expected.from_user_id == actual.from_user_id && expected.to_user_id == actual.to_user_id &&
           expected.content == actual.content;
}

}  // namespace

ChatService::ChatService(ISessionManager& session_manager, IMessageRepository& message_repository,
                         IUserRepository& user_repository, IRateLimiter* rate_limiter, IMessageDedupCache* dedup_cache,
                         RedisConfig config, IGlobalSessionStore* global_session_store,
                         IRemotePushPublisher* remote_push_publisher)
    : session_manager_(session_manager),
      message_repository_(message_repository),
      user_repository_(user_repository),
      rate_limiter_(rate_limiter),
      dedup_cache_(dedup_cache),
      redis_config_(std::move(config)),
      global_session_store_(global_session_store),
      remote_push_publisher_(remote_push_publisher) {}

SendMessageResult ChatService::sendMessage(ConnectionId from_conn_id, const SendMessageRequest& req) {
    // 0. 校验内容合法性（安全与业务兜底）
    if (req.content.empty()) {
        SendMessageResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "Message content cannot be empty";
        return result;
    }
    if (req.content.length() > 4096) {
        SendMessageResult result;
        result.code = ErrorCode::MESSAGE_TOO_LONG;
        result.message = "Message content exceeds limit (4096)";
        return result;
    }
    if (req.client_msg_id.empty()) {
        SendMessageResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "client_msg_id cannot be empty";
        return result;
    }
    if (req.client_msg_id.length() > 64) {
        SendMessageResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "client_msg_id exceeds limit (64)";
        return result;
    }
    if (req.to_user_id <= 0) {
        SendMessageResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "to_user_id must be greater than 0";
        return result;
    }

    // 1. 获取发送方会话
    auto from_session_opt = session_manager_.GetSession(from_conn_id);

    // 2. 如果没有 session，返回 user not logged in
    if (!from_session_opt.has_value() || !from_session_opt->authenticated) {
        SendMessageResult result;
        result.code = ErrorCode::NOT_LOGGED_IN;
        result.message = "User not logged in";
        return result;
    }

    // 3. 如果 req.to_user_id == 当前用户 id，返回 CANNOT_SEND_TO_SELF
    if (req.to_user_id == from_session_opt->user_id) {
        SendMessageResult result;
        result.code = ErrorCode::CANNOT_SEND_TO_SELF;
        result.message = "Cannot send message to yourself";
        return result;
    }
    if (rate_limiter_ != nullptr) {
        const RateLimitResult limit =
            rate_limiter_->Allow("send", std::to_string(from_session_opt->user_id), redis_config_.send_rate_limit,
                                 redis_config_.send_rate_window_seconds);
        if (!limit.allowed) {
            SendMessageResult result;
            result.code = ErrorCode::RATE_LIMITED;
            result.message = "send message rate limit exceeded";
            result.retry_after_seconds = limit.retry_after_seconds;
            return result;
        }
    }

    const FindUserResult target_user = user_repository_.findById(req.to_user_id);
    if (target_user.status == RepositoryStatus::kNotFound ||
        (target_user.status == RepositoryStatus::kOk && !target_user.user.has_value())) {
        SendMessageResult result;
        result.code = ErrorCode::USER_NOT_FOUND;
        result.message = "target user not found";
        return result;
    }
    if (target_user.status != RepositoryStatus::kOk) {
        SendMessageResult result;
        result.code = MapRepositoryError(target_user.status);
        result.message = "query target user failed";
        return result;
    }

    const Timestamp now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());

    MessageRecord pending_message;
    pending_message.id = BuildMessageId(now, from_session_opt->user_id);
    pending_message.conversation_id = BuildConversationId(from_session_opt->user_id, req.to_user_id);
    pending_message.client_msg_id = req.client_msg_id;
    pending_message.from_user_id = from_session_opt->user_id;
    pending_message.to_user_id = req.to_user_id;
    pending_message.content = req.content;
    pending_message.status = MessageStatus::kStored;
    pending_message.created_at = now;

    CreateMessageResult create_result;
    const std::optional<std::string> cached_message_id =
        dedup_cache_ == nullptr ? std::nullopt : dedup_cache_->Lookup(from_session_opt->user_id, req.client_msg_id);
    if (cached_message_id) {
        const FindMessageResult existing =
            message_repository_.findMessageByClientMsgId(from_session_opt->user_id, req.client_msg_id);
        if (existing.status == RepositoryStatus::kOk && existing.message &&
            existing.message->id == *cached_message_id) {
            create_result = {.status = RepositoryStatus::kOk,
                             .message_id = existing.message->id,
                             .message = existing.message,
                             .created = false};
        } else {
            dedup_cache_->Remove(from_session_opt->user_id, req.client_msg_id);
        }
    }
    if (!create_result.message) {
        create_result = message_repository_.createMessage(pending_message);
    }
    if (create_result.status != RepositoryStatus::kOk || !create_result.message.has_value()) {
        SendMessageResult result;
        result.code = MapRepositoryError(create_result.status);
        result.message = "store message failed";
        return result;
    }

    MessageRecord stored_message = *create_result.message;
    if (!IsSameIdempotentMessage(pending_message, stored_message)) {
        SendMessageResult result;
        result.code = ErrorCode::IDEMPOTENCY_CONFLICT;
        result.message = "client_msg_id conflicts with existing message";
        return result;
    }
    if (dedup_cache_ != nullptr) {
        dedup_cache_->Save(from_session_opt->user_id, req.client_msg_id, stored_message.id);
    }

    const bool can_attempt_push = create_result.created || stored_message.status == MessageStatus::kStored;
    std::optional<ConnectionId> target_conn_id;
    std::optional<StoredUserPresence> presence;
    if (can_attempt_push && global_session_store_ != nullptr) {
        presence = global_session_store_->GetPresence(stored_message.to_user_id);
    }
    if (can_attempt_push && presence) {
        if (presence->server_id == redis_config_.server_id) {
            target_conn_id = session_manager_.GetConnectionId(stored_message.to_user_id);
            if (target_conn_id != presence->connection_id) {
                target_conn_id.reset();
            }
        }
    } else if (can_attempt_push) {
        // Redis 不可用或未启用时保留原有单进程推送路径。
        target_conn_id = session_manager_.GetConnectionId(stored_message.to_user_id);
    }

    // 6. 返回发送方、接收方、目标连接、消息内容等信息
    SendMessageResult result;
    result.code = ErrorCode::OK;
    result.message = "Success";
    result.from_user_id = from_session_opt->user_id;
    result.from_username = from_session_opt->username;
    result.to_user_id = stored_message.to_user_id;
    result.to_conn_id = target_conn_id.value_or(0);
    if (can_attempt_push && presence && presence->server_id != redis_config_.server_id) {
        result.remote_server_id = presence->server_id;
        result.remote_conn_id = presence->connection_id;
    }
    result.content = stored_message.content;
    result.server_time = now;
    result.message_id = stored_message.id;
    result.conversation_id = stored_message.conversation_id;
    result.status = ToProtocolMessageStatus(stored_message.status);
    result.created_at = stored_message.created_at;

    return result;
}

bool ChatService::publishRemotePush(const SendMessageResult& result, const std::string& payload) {
    if (remote_push_publisher_ == nullptr || result.remote_server_id.empty() || result.remote_conn_id == 0 ||
        result.message_id.empty() || payload.empty()) {
        return false;
    }
    RemotePushEvent event;
    event.event_id = result.message_id;
    event.message_id = result.message_id;
    event.from_user_id = result.from_user_id;
    event.to_user_id = result.to_user_id;
    event.target_connection_id = result.remote_conn_id;
    event.payload = payload;
    return remote_push_publisher_->Publish(result.remote_server_id, event);
}

PullOfflineMessagesResult ChatService::pullOfflineMessages(ConnectionId from_conn_id,
                                                           const PullOfflineMessagesRequest& req) {
    // 1. 获取发送方会话
    auto from_session_opt = session_manager_.GetSession(from_conn_id);

    // 2. 如果没有 session，返回 user not logged in
    if (!from_session_opt.has_value() || !from_session_opt->authenticated) {
        PullOfflineMessagesResult result;
        result.code = ErrorCode::NOT_LOGGED_IN;
        result.message = "User not logged in";
        return result;
    }
    if (!req.before_message_id.empty()) {
        PullOfflineMessagesResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "before_message_id is not supported";
        return result;
    }
    if (req.limit <= 0 || req.limit > kMaxPullOfflineLimit) {
        PullOfflineMessagesResult result;
        result.code = ErrorCode::INVALID_PARAM;
        result.message = "limit must be between 1 and 100";
        return result;
    }

    const int32_t limit = req.limit;
    ListOfflineMessagesResult list_result =
        message_repository_.listOfflineMessages(from_session_opt->user_id, limit, req.since_message_id);
    if (list_result.status != RepositoryStatus::kOk) {
        PullOfflineMessagesResult result;
        result.code = MapRepositoryError(list_result.status);
        result.message = "list offline messages failed";
        return result;
    }

    PullOfflineMessagesResult result;
    result.code = ErrorCode::OK;
    result.message = "Success";
    result.has_more = list_result.has_more;
    for (const MessageRecord& record : list_result.messages) {
        OfflineMessage message;
        message.message_id = record.id;
        message.conversation_id = record.conversation_id;
        message.from_user_id = record.from_user_id;
        message.to_user_id = record.to_user_id;
        message.content = record.content;
        message.created_at = record.created_at;
        message.status = ToProtocolMessageStatus(record.status);
        result.messages.push_back(message);
    }
    return result;
}

void ChatService::markMessagesDelivered(UserId user_id, const std::vector<std::string>& message_ids) {
    message_repository_.markDelivered(user_id, message_ids);
}

}  // namespace chat
