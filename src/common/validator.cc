#include "common/validator.h"

#include <cctype>
#include <cstddef>
#include <cstdint>

namespace {

chat::ValidationResult Valid() { return {.valid = true}; }

chat::ValidationResult Invalid(const std::string& message) { return {.valid = false, .message = message}; }

bool HasNul(const std::string& value) { return value.find('\0') != std::string::npos; }

bool IsValidIdentifier(const std::string& value) {
    for (const unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    }
    return true;
}

bool DecodeUtf8(const std::string& value, std::size_t* code_points, bool* has_control) {
    *code_points = 0;
    *has_control = false;
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char lead = static_cast<unsigned char>(value[i]);
        std::uint32_t code_point = 0;
        std::size_t length = 0;
        if (lead <= 0x7f) {
            code_point = lead;
            length = 1;
        } else if ((lead & 0xe0) == 0xc0) {
            code_point = lead & 0x1f;
            length = 2;
        } else if ((lead & 0xf0) == 0xe0) {
            code_point = lead & 0x0f;
            length = 3;
        } else if ((lead & 0xf8) == 0xf0) {
            code_point = lead & 0x07;
            length = 4;
        } else {
            return false;
        }
        if (i + length > value.size()) {
            return false;
        }
        for (std::size_t j = 1; j < length; ++j) {
            const unsigned char continuation = static_cast<unsigned char>(value[i + j]);
            if ((continuation & 0xc0) != 0x80) {
                return false;
            }
            code_point = (code_point << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && code_point < 0x80) || (length == 3 && code_point < 0x800) ||
            (length == 4 && code_point < 0x10000) || code_point > 0x10ffff ||
            (code_point >= 0xd800 && code_point <= 0xdfff)) {
            return false;
        }
        if (code_point <= 0x1f || (code_point >= 0x7f && code_point <= 0x9f)) {
            *has_control = true;
        }
        ++*code_points;
        i += length;
    }
    return true;
}

chat::ValidationResult RequiredIdentifier(const std::string& value, const std::string& field) {
    if (value.empty()) {
        return Invalid(field + " is empty");
    }
    if (value.size() > chat::Validator::kIdMaxLength) {
        return Invalid(field + " exceeds maximum length");
    }
    if (!IsValidIdentifier(value)) {
        return Invalid(field + " contains invalid characters");
    }
    return Valid();
}

}  // namespace

namespace chat {

ValidationResult Validator::Username(const std::string& username) {
    if (username.size() < kUsernameMinLength || username.size() > kUsernameMaxLength) {
        return Invalid("username length must be between 3 and 32");
    }
    if (!IsValidIdentifier(username)) {
        return Invalid("username contains invalid characters");
    }
    return Valid();
}

ValidationResult Validator::RegisterPassword(const std::string& password) {
    if (HasNul(password)) {
        return Invalid("password contains NUL");
    }
    if (password.size() < kRegisterPasswordMinLength || password.size() > kPasswordMaxLength) {
        return Invalid("password length must be between 6 and 72");
    }
    return Valid();
}

ValidationResult Validator::LoginPassword(const std::string& password) {
    if (HasNul(password)) {
        return Invalid("password contains NUL");
    }
    if (password.empty() || password.size() > kPasswordMaxLength) {
        return Invalid("password length must be between 1 and 72");
    }
    return Valid();
}

ValidationResult Validator::Nickname(const std::string& nickname) {
    if (nickname.empty()) {
        return Invalid("nickname is empty");
    }
    std::size_t code_points = 0;
    bool has_control = false;
    if (!DecodeUtf8(nickname, &code_points, &has_control)) {
        return Invalid("nickname is not valid UTF-8");
    }
    if (code_points > kNicknameMaxLength) {
        return Invalid("nickname exceeds maximum length");
    }
    if (has_control) {
        return Invalid("nickname contains control characters");
    }
    return Valid();
}

ValidationResult Validator::MessageContent(const std::string& content) {
    if (content.empty()) {
        return Invalid("message content is empty");
    }
    if (content.size() > kMessageMaxBytes) {
        return Invalid("message content exceeds maximum length");
    }
    if (HasNul(content)) {
        return Invalid("message content contains NUL");
    }
    return Valid();
}

ValidationResult Validator::ClientMessageId(const std::string& client_msg_id) {
    return RequiredIdentifier(client_msg_id, "client_msg_id");
}

ValidationResult Validator::MessageId(const std::string& message_id) {
    return RequiredIdentifier(message_id, "message_id");
}

ValidationResult Validator::ConversationId(const std::string& conversation_id) {
    return RequiredIdentifier(conversation_id, "conversation_id");
}

ValidationResult Validator::Cursor(const std::string& cursor) {
    if (cursor.empty()) {
        return Valid();
    }
    return RequiredIdentifier(cursor, "cursor");
}

ValidationResult Validator::Token(const std::string& token) {
    if (token.size() != 64) {
        return Invalid("token must contain 64 hexadecimal characters");
    }
    for (const unsigned char ch : token) {
        if (!std::isxdigit(ch)) {
            return Invalid("token must contain 64 hexadecimal characters");
        }
    }
    return Valid();
}

}  // namespace chat
