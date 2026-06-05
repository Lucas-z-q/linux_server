#ifndef LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_
#define LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "db/message_repository.h"

class FakeMessageRepository : public chat::IMessageRepository {
   public:
    chat::RepositoryStatus create_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus find_or_create_conv_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus find_by_client_msg_id_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus mark_delivered_status = chat::RepositoryStatus::kOk;
    chat::RepositoryStatus mark_read_status = chat::RepositoryStatus::kOk;
    int mark_delivered_fail_after = -1;
    chat::ListOfflineMessagesResult list_result;
    std::optional<chat::MessageRecord> create_message_override;
    bool create_result_created = true;
    int create_message_calls = 0;
    int find_by_client_msg_id_calls = 0;
    std::vector<chat::MessageRecord> created_messages;
    std::vector<std::string> delivered_message_ids;
    std::vector<std::string> read_message_ids;
    std::vector<std::string> existing_conversations;
    std::string last_before_message_id;  // Keep for field compatibility, though unused in the new listOfflineMessages
    std::string last_since_message_id;

    FakeMessageRepository() { list_result.status = chat::RepositoryStatus::kOk; }

    chat::FindOrCreateConversationResult findOrCreateSingleConversation(chat::UserId user_a,
                                                                        chat::UserId user_b) override {
        if (find_or_create_conv_status != chat::RepositoryStatus::kOk) {
            return {.status = find_or_create_conv_status};
        }
        const chat::UserId min_id = std::min(user_a, user_b);
        const chat::UserId max_id = std::max(user_a, user_b);
        std::string conv_id = "conv_" + std::to_string(min_id) + "_" + std::to_string(max_id);

        bool already_exists = false;
        for (const auto& existing_id : existing_conversations) {
            if (existing_id == conv_id) {
                already_exists = true;
                break;
            }
        }
        if (!already_exists) {
            existing_conversations.push_back(conv_id);
        }
        return {.status = chat::RepositoryStatus::kOk, .conversation_id = conv_id, .created = !already_exists};
    }

    chat::CreateMessageResult createMessage(const chat::MessageRecord& message) override {
        ++create_message_calls;
        if (create_status != chat::RepositoryStatus::kOk) {
            return {.status = create_status};
        }
        if (create_message_override.has_value()) {
            return {.status = chat::RepositoryStatus::kOk,
                    .message_id = create_message_override->id,
                    .message = create_message_override,
                    .created = false};
        }
        for (const auto& existing : created_messages) {
            if (existing.from_user_id == message.from_user_id && existing.client_msg_id == message.client_msg_id) {
                return {.status = chat::RepositoryStatus::kOk,
                        .message_id = existing.id,
                        .message = existing,
                        .created = false};
            }
        }
        created_messages.push_back(message);
        return {.status = chat::RepositoryStatus::kOk,
                .message_id = message.id,
                .message = message,
                .created = create_result_created};
    }

    chat::FindMessageResult findMessageByClientMsgId(chat::UserId from_user_id,
                                                     const std::string& client_msg_id) override {
        ++find_by_client_msg_id_calls;
        if (find_by_client_msg_id_status != chat::RepositoryStatus::kOk) {
            return {.status = find_by_client_msg_id_status};
        }
        for (const auto& msg : created_messages) {
            if (msg.from_user_id == from_user_id && msg.client_msg_id == client_msg_id) {
                return {.status = chat::RepositoryStatus::kOk, .message = msg};
            }
        }
        return {.status = chat::RepositoryStatus::kNotFound};
    }

    chat::ListOfflineMessagesResult listOfflineMessages(chat::UserId to_user_id, int32_t limit,
                                                        const std::string& cursor) override {
        last_since_message_id = cursor;
        if (list_result.status != chat::RepositoryStatus::kOk || !list_result.messages.empty() ||
            list_result.has_more) {
            return list_result;
        }

        chat::ListOfflineMessagesResult result;
        result.status = chat::RepositoryStatus::kOk;
        bool after_cursor = cursor.empty();
        for (const auto& msg : created_messages) {
            if (!after_cursor) {
                if (msg.id == cursor) {
                    after_cursor = true;
                }
                continue;
            }
            if (msg.to_user_id != to_user_id || msg.status != chat::MessageStatus::kStored) {
                continue;
            }
            if (static_cast<int32_t>(result.messages.size()) >= limit) {
                result.has_more = true;
                break;
            }
            result.messages.push_back(msg);
        }
        return result;
    }

    chat::MarkDeliveredResult markDelivered(chat::UserId to_user_id,
                                            const std::vector<std::string>& message_ids) override {
        if (mark_delivered_status != chat::RepositoryStatus::kOk) {
            return {.status = mark_delivered_status};
        }
        int affected = 0;
        for (const auto& id : message_ids) {
            delivered_message_ids.push_back(id);
            if (mark_delivered_fail_after >= 0 &&
                static_cast<int>(delivered_message_ids.size()) > mark_delivered_fail_after) {
                return {.status = chat::RepositoryStatus::kQueryFailed, .affected_rows = affected};
            }
            for (auto& msg : created_messages) {
                if (msg.id == id && msg.to_user_id == to_user_id && msg.status == chat::MessageStatus::kStored) {
                    msg.status = chat::MessageStatus::kDelivered;
                    affected++;
                    break;
                }
            }
            for (auto& msg : list_result.messages) {
                if (msg.id == id && msg.to_user_id == to_user_id && msg.status == chat::MessageStatus::kStored) {
                    msg.status = chat::MessageStatus::kDelivered;
                    break;
                }
            }
        }
        return {.status = chat::RepositoryStatus::kOk, .affected_rows = affected};
    }

    chat::MarkReadResult markRead(chat::UserId to_user_id, const std::vector<std::string>& message_ids) override {
        (void)to_user_id;
        if (mark_read_status != chat::RepositoryStatus::kOk) {
            return {.status = mark_read_status};
        }
        int affected = 0;
        for (const auto& id : message_ids) {
            read_message_ids.push_back(id);
            affected++;
        }
        return {.status = chat::RepositoryStatus::kOk, .affected_rows = affected};
    }
};

#endif  // LINUX_SERVER_TESTS_FAKE_MESSAGE_REPOSITORY_H_
