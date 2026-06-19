#ifndef LINUX_SERVER_INCLUDE_COMMON_VALIDATOR_H_
#define LINUX_SERVER_INCLUDE_COMMON_VALIDATOR_H_

#include <cstddef>
#include <string>

namespace chat {

struct ValidationResult {
    bool valid = false;
    std::string message;

    bool ok() const noexcept { return valid; }
};

class Validator {
   public:
    static constexpr std::size_t kUsernameMinLength = 3;
    static constexpr std::size_t kUsernameMaxLength = 32;
    static constexpr std::size_t kRegisterPasswordMinLength = 6;
    static constexpr std::size_t kPasswordMaxLength = 72;
    static constexpr std::size_t kNicknameMaxLength = 64;
    static constexpr std::size_t kMessageMaxBytes = 4096;
    static constexpr std::size_t kIdMaxLength = 64;

    static ValidationResult Username(const std::string& username);
    static ValidationResult RegisterPassword(const std::string& password);
    static ValidationResult LoginPassword(const std::string& password);
    static ValidationResult Nickname(const std::string& nickname);
    static ValidationResult MessageContent(const std::string& content);
    static ValidationResult ClientMessageId(const std::string& client_msg_id);
    static ValidationResult MessageId(const std::string& message_id);
    static ValidationResult ConversationId(const std::string& conversation_id);
    static ValidationResult Cursor(const std::string& cursor);
    static ValidationResult Token(const std::string& token);
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_COMMON_VALIDATOR_H_
