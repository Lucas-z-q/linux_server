#ifndef LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_
#define LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_

#include <optional>
#include <string>
#include <vector>

#include "db/message_repository.h"

class FakeMessageRepository : public chat::IMessageRepository {
   public:
    chat::RepositoryStatus create_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus mark_delivered_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus mark_read_status = chat::RepositoryStatus::kOk;
    int mark_delivered_fail_after = -1;
    chat::ListOfflineMessagesResult list_result;
    std::optional<chat::MessageRecord> create_message_override;
    bool create_result_created = true;
    std::vector<chat::MessageRecord> created_messages;
    std::vector<std::string> delivered_message_ids;
    std::vector<std::string> read_message_ids;
    std::string last_before_message_id;
    std::string last_since_message_id;

    FakeMessageRepository() { list_result.status = chat::RepositoryStatus::kOk; }

    chat::CreateMessageResult createMessage(const chat::MessageRecord& message) override {
        if (create_status != chat::RepositoryStatus::kOk) {
            return {.status = create_status};
        }
        created_messages.push_back(message);
        if (create_message_override.has_value()) {
            return {.status = chat::RepositoryStatus::kOk, .message = create_message_override, .created = false};
        }
        return {.status = chat::RepositoryStatus::kOk, .message = message, .created = create_result_created};
    }

    chat::ListOfflineMessagesResult listOfflineMessages(chat::UserId to_user_id, int32_t limit,
                                                        const std::string& before_message_id,
                                                        const std::string& since_message_id) override {
        (void)to_user_id;
        (void)limit;
        last_before_message_id = before_message_id;
        last_since_message_id = since_message_id;
        return list_result;
    }

    chat::RepositoryStatus markDelivered(const std::string& message_id, chat::Timestamp delivered_at) override {
        (void)delivered_at;
        delivered_message_ids.push_back(message_id);
        if (mark_delivered_fail_after >= 0 &&
            static_cast<int>(delivered_message_ids.size()) > mark_delivered_fail_after) {
            return chat::RepositoryStatus::kQueryFailed;
        }
        return mark_delivered_status;
    }

    chat::RepositoryStatus markRead(const std::string& message_id, chat::Timestamp read_at) override {
        (void)read_at;
        read_message_ids.push_back(message_id);
        return mark_read_status;
    }
};

#endif  // LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_
