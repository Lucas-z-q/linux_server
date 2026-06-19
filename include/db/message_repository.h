#ifndef LINUX_SERVER_INCLUDE_DB_MESSAGE_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_DB_MESSAGE_REPOSITORY_H_

#include <optional>
#include <string>
#include <vector>

#include "common/types.h"
#include "db/user_repository.h"
#include "model/message_record.h"

namespace chat {

struct CreateMessageResult {
    RepositoryStatus status = RepositoryStatus::kInsertFailed;
    std::string message_id;
    std::optional<MessageRecord> message;
    bool created = false;
};

struct ListOfflineMessagesResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::vector<MessageRecord> messages;
    bool has_more = false;
};

struct MarkDeliveredResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    int32_t affected_rows = 0;
};

struct MarkReadResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    int32_t affected_rows = 0;
};

struct FindOrCreateConversationResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::string conversation_id;
    bool created = false;
};

struct FindMessageResult {
    RepositoryStatus status = RepositoryStatus::kNotFound;
    std::optional<MessageRecord> message;
};

class IMessageRepository {
   public:
    virtual ~IMessageRepository() = default;

    virtual FindOrCreateConversationResult findOrCreateSingleConversation(UserId user_a, UserId user_b) = 0;
    virtual CreateMessageResult createMessage(const MessageRecord& message) = 0;
    virtual FindMessageResult findMessageByClientMsgId(UserId from_user_id, const std::string& client_msg_id) = 0;
    virtual ListOfflineMessagesResult listOfflineMessages(UserId to_user_id, int32_t limit,
                                                          const std::string& cursor) = 0;
    virtual MarkDeliveredResult markDelivered(UserId to_user_id, const std::vector<std::string>& message_ids) = 0;
    virtual MarkReadResult markRead(UserId to_user_id, const std::vector<std::string>& message_ids) = 0;
};

class DbPool;

class MessageRepository : public IMessageRepository {
   public:
    explicit MessageRepository(DbPool* pool);

    FindOrCreateConversationResult findOrCreateSingleConversation(UserId user_a, UserId user_b) override;
    CreateMessageResult createMessage(const MessageRecord& message) override;
    FindMessageResult findMessageByClientMsgId(UserId from_user_id, const std::string& client_msg_id) override;
    ListOfflineMessagesResult listOfflineMessages(UserId to_user_id, int32_t limit, const std::string& cursor) override;
    MarkDeliveredResult markDelivered(UserId to_user_id, const std::vector<std::string>& message_ids) override;
    MarkReadResult markRead(UserId to_user_id, const std::vector<std::string>& message_ids) override;

   private:
    DbPool* pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_MESSAGE_REPOSITORY_H_
