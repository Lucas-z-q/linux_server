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
    std::optional<MessageRecord> message;
    bool created = false;
};

struct ListOfflineMessagesResult {
    RepositoryStatus status = RepositoryStatus::kQueryFailed;
    std::vector<MessageRecord> messages;
    bool has_more = false;
};

class IMessageRepository {
   public:
    virtual ~IMessageRepository() = default;

    virtual CreateMessageResult createMessage(const MessageRecord& message) = 0;
    virtual ListOfflineMessagesResult listOfflineMessages(UserId to_user_id, int32_t limit,
                                                          const std::string& before_message_id,
                                                          const std::string& since_message_id) = 0;
    virtual RepositoryStatus markDelivered(const std::string& message_id, Timestamp delivered_at) = 0;
    virtual RepositoryStatus markRead(const std::string& message_id, Timestamp read_at) = 0;
};

class DbPool;

class MessageRepository : public IMessageRepository {
   public:
    explicit MessageRepository(DbPool* pool);

    CreateMessageResult createMessage(const MessageRecord& message) override;
    ListOfflineMessagesResult listOfflineMessages(UserId to_user_id, int32_t limit,
                                                  const std::string& before_message_id,
                                                  const std::string& since_message_id) override;
    RepositoryStatus markDelivered(const std::string& message_id, Timestamp delivered_at) override;
    RepositoryStatus markRead(const std::string& message_id, Timestamp read_at) override;

   private:
    DbPool* pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_MESSAGE_REPOSITORY_H_
