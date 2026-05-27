#include "model/message_record.h"

#include <cassert>
#include <iostream>

void TestMessageStatusStorageValues() {
    assert(chat::ToStorageMessageStatus(chat::MessageStatus::kStored) == 0);
    assert(chat::ToStorageMessageStatus(chat::MessageStatus::kDelivered) == 1);
    assert(chat::ToStorageMessageStatus(chat::MessageStatus::kRead) == 2);
}

void TestParseMessageStatus() {
    chat::MessageStatus status = chat::MessageStatus::kStored;
    assert(chat::ParseMessageStatus(0, &status));
    assert(status == chat::MessageStatus::kStored);
    assert(chat::ParseMessageStatus(1, &status));
    assert(status == chat::MessageStatus::kDelivered);
    assert(chat::ParseMessageStatus(2, &status));
    assert(status == chat::MessageStatus::kRead);
    assert(!chat::ParseMessageStatus(3, &status));
    assert(!chat::ParseMessageStatus(0, nullptr));
}

void TestMessageStatusTransitions() {
    assert(chat::CanTransitionMessageStatus(chat::MessageStatus::kStored, chat::MessageStatus::kDelivered));
    assert(chat::CanTransitionMessageStatus(chat::MessageStatus::kStored, chat::MessageStatus::kRead));
    assert(chat::CanTransitionMessageStatus(chat::MessageStatus::kDelivered, chat::MessageStatus::kRead));
    assert(chat::CanTransitionMessageStatus(chat::MessageStatus::kDelivered, chat::MessageStatus::kDelivered));
    assert(chat::CanTransitionMessageStatus(chat::MessageStatus::kRead, chat::MessageStatus::kRead));

    assert(!chat::CanTransitionMessageStatus(chat::MessageStatus::kStored, chat::MessageStatus::kStored));
    assert(!chat::CanTransitionMessageStatus(chat::MessageStatus::kDelivered, chat::MessageStatus::kStored));
    assert(!chat::CanTransitionMessageStatus(chat::MessageStatus::kRead, chat::MessageStatus::kStored));
    assert(!chat::CanTransitionMessageStatus(chat::MessageStatus::kRead, chat::MessageStatus::kDelivered));
}

void TestConstructMessageRecord() {
    chat::MessageRecord record;
    record.id = "m1";
    record.conversation_id = "conv_1_2";
    record.client_msg_id = "c1";
    record.from_user_id = 1;
    record.to_user_id = 2;
    record.content = "hello";
    record.status = chat::MessageStatus::kStored;
    record.created_at = 123;

    assert(record.id == "m1");
    assert(record.status == chat::MessageStatus::kStored);
    assert(chat::ToProtocolMessageStatus(record.status) == 0);
}

int main() {
    TestMessageStatusStorageValues();
    TestParseMessageStatus();
    TestMessageStatusTransitions();
    TestConstructMessageRecord();
    std::cout << "[PASS] message record tests passed\n";
    return 0;
}
