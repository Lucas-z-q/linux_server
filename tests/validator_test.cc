#include "common/validator.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Case {
    std::string value;
    bool valid;
};

void TestUsernameValidation() {
    const std::vector<Case> cases = {
        {"abc", true},
        {std::string(32, 'a'), true},
        {"", false},
        {"ab", false},
        {std::string(33, 'a'), false},
        {"bad-name", true},
        {"bad name", false},
        {"bad'name", false},
        {"bad\"name", false},
        {"bad\\name", false},
        {"bad@name", false},
        {"' OR 1=1 --", false},
        {"\xe7\x94\xa8\xe6\x88\xb7", false},
    };
    for (const auto& test : cases) {
        assert(chat::Validator::Username(test.value).ok() == test.valid);
    }
}

void TestPasswordValidation() {
    const std::vector<Case> register_cases = {
        {std::string(6, 'a'), true},   {std::string(72, 'a'), true},        {std::string(5, 'a'), false}, {"", false},
        {std::string(73, 'a'), false}, {std::string("abc\0def", 7), false},
    };
    for (const auto& test : register_cases) {
        assert(chat::Validator::RegisterPassword(test.value).ok() == test.valid);
    }

    const std::vector<Case> login_cases = {
        {"a", true},
        {std::string(72, 'a'), true},
        {"", false},
        {std::string(73, 'a'), false},
        {std::string("abc\0def", 7), false},
    };
    for (const auto& test : login_cases) {
        assert(chat::Validator::LoginPassword(test.value).ok() == test.valid);
    }
}

void TestNicknameValidation() {
    std::string unicode_max;
    for (int i = 0; i < 64; ++i) {
        unicode_max += "\xe7\x94\xa8";
    }
    const std::vector<Case> cases = {
        {"", false},
        {std::string(64, 'a'), true},
        {std::string(65, 'a'), false},
        {"line\nbreak", false},
        {std::string("bad\0name", 8), false},
        {unicode_max, true},
        {unicode_max + "\xe7\x94\xa8", false},
        {"\xe6\xb5\x8b\xe8\xaf\x95\xe7\x94\xa8\xe6\x88\xb7", true},
    };
    for (const auto& test : cases) {
        assert(chat::Validator::Nickname(test.value).ok() == test.valid);
    }
}

void TestMessageContentValidation() {
    const std::vector<Case> cases = {
        {"", false},
        {"x", true},
        {std::string(4096, 'x'), true},
        {std::string(4097, 'x'), false},
        {"quote'\" slash\\ newline\n", true},
        {"\xe4\xbd\xa0\xe5\xa5\xbd", true},
        {std::string("abc\0def", 7), false},
    };
    for (const auto& test : cases) {
        assert(chat::Validator::MessageContent(test.value).ok() == test.valid);
    }
}

void TestIdentifierValidation() {
    const std::vector<Case> required_cases = {
        {"id", true},      {std::string(64, 'a'), true}, {"", false},       {std::string(65, 'a'), false},
        {"bad'id", false}, {"bad\\id", false},           {"bad id", false},
    };
    for (const auto& test : required_cases) {
        assert(chat::Validator::ClientMessageId(test.value).ok() == test.valid);
        assert(chat::Validator::MessageId(test.value).ok() == test.valid);
        assert(chat::Validator::ConversationId(test.value).ok() == test.valid);
    }
    assert(chat::Validator::Cursor("").ok());
    assert(chat::Validator::Cursor("msg_123").ok());
    assert(!chat::Validator::Cursor("bad'cursor").ok());
    assert(!chat::Validator::Cursor(std::string(65, 'a')).ok());
}

}  // namespace

int main() {
    TestUsernameValidation();
    TestPasswordValidation();
    TestNicknameValidation();
    TestMessageContentValidation();
    TestIdentifierValidation();
    std::cout << "[PASS] validator tests passed\n";
    return 0;
}
