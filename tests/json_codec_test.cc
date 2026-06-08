#include "codec/json_codec.h"

#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

#include "common/message.h"
#include "common/response.h"

using namespace chat;

namespace {

void TestDecodeAndParseLoginRequest() {
    JsonCodec codec;
    const std::string raw_login =
        R"({"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}})";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(raw_login, msg, err);
    assert(decode_ok);
    assert(msg.msg_type == "login");
    assert(msg.seq == 2);

    LoginRequest req;
    const bool parse_ok = codec.parseLoginRequest(msg, req, err);
    assert(parse_ok);
    assert(req.username == "alice");
    assert(req.password == "123456");
}

void TestRejectInvalidJson() {
    JsonCodec codec;
    const std::string bad_json = R"({"msg_type":"login", "seq": 3, "data": { broken...)";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(bad_json, msg, err);
    assert(!decode_ok);
}

void TestRejectMissingRequiredField() {
    JsonCodec codec;
    const std::string missing_field_json = R"({"msg_type":"login","seq":4,"data":{"username":"alice"}})";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(missing_field_json, msg, err);
    assert(decode_ok);

    LoginRequest req;
    const bool parse_ok = codec.parseLoginRequest(msg, req, err);
    assert(!parse_ok);
}

void TestRejectInvalidTokenType() {
    JsonCodec codec;
    const std::string bad_token_json = R"({"msg_type":"heartbeat","seq":5,"token":123,"data":{}})";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(bad_token_json, msg, err);

    assert(!decode_ok);
    assert(err.find("token") != std::string::npos);
}

void TestEncodeResponseReturnsPureJson() {
    JsonCodec codec;
    Response resp;
    resp.msg_type = "login_resp";
    resp.seq = 2;
    resp.code = ErrorCode::OK;
    resp.message = "login success";

    LoginResponseData data;
    data.user_id = 10001;
    data.nickname = "Alice";
    data.token = "token_xxx";

    codec.fillLoginResponse(resp, data);

    const std::string out_json = codec.encodeResponse(resp);
    assert(!out_json.empty());
    assert(out_json.back() != '\n');

    const nlohmann::json encoded = nlohmann::json::parse(out_json);
    assert(encoded["msg_type"].get<std::string>() == "login_resp");
    assert(encoded["seq"].get<int>() == 2);
    assert(encoded["code"].get<int>() == static_cast<int>(ErrorCode::OK));
    assert(encoded["data"]["user_id"].get<int>() == 10001);
    assert(encoded["data"]["nickname"].get<std::string>() == "Alice");
    assert(encoded["data"]["token"].get<std::string>() == "token_xxx");
}

void TestSendMessageRequest() {
    JsonCodec codec;
    std::string err;

    // 1. Success case
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.seq = 100;
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 12345;
        msg.data["content"] = "hello world";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(ok);
        assert(req.client_msg_id == "msg_123");
        assert(req.to_user_id == 12345);
        assert(req.content == "hello world");
    }

    // 2. Missing to_user_id
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("to_user_id") != std::string::npos);
    }

    // 3. invalid to_user_id type (not integer)
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = "12345";
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("to_user_id") != std::string::npos);
    }

    // 4. invalid to_user_id value (<= 0)
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 0;
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("to_user_id") != std::string::npos);
    }

    // 5. Missing content
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 123;

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("content") != std::string::npos);
    }

    // 6. Invalid content type
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 123;
        msg.data["content"] = 12345;

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("content") != std::string::npos);
    }

    // 7. Empty content
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 123;
        msg.data["content"] = "";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("Empty") != std::string::npos);
    }

    // 8. Too long content (> 4096)
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "msg_123";
        msg.data["to_user_id"] = 123;
        msg.data["content"] = std::string(4097, 'a');

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("too long") != std::string::npos);
    }

    // 8a. Missing client_msg_id
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["to_user_id"] = 123;
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("client_msg_id") != std::string::npos);
    }

    // 8b. Empty client_msg_id
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = "";
        msg.data["to_user_id"] = 123;
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("Empty") != std::string::npos);
    }

    // 8c. Too long client_msg_id (> 64)
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
        msg.data["client_msg_id"] = std::string(65, 'a');
        msg.data["to_user_id"] = 123;
        msg.data["content"] = "hello";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("too long") != std::string::npos);
    }

    // 9. fillSendMessageAck
    {
        Response resp;
        SendMessageAckData data;
        data.message_id = "msg_1";
        data.conversation_id = "conv_1";
        data.to_user_id = 999;
        data.status = 1;
        data.created_at = 123456;

        codec.fillSendMessageAck(resp, data);
        assert(resp.data["message_id"].get<std::string>() == "msg_1");
        assert(resp.data["conversation_id"].get<std::string>() == "conv_1");
        assert(resp.data["to_user_id"].get<UserId>() == 999);
        assert(resp.data["status"].get<int32_t>() == 1);
        assert(resp.data["created_at"].get<Timestamp>() == 123456);
    }

    // 10. fillMessagePush
    {
        Response resp;
        MessagePushData data;
        data.message_id = "msg_2";
        data.conversation_id = "conv_2";
        data.from_user_id = 111;
        data.from_username = "alice";
        data.to_user_id = 222;
        data.content = "hi";
        data.created_at = 1621948800;
        data.server_time = 1621948800;

        codec.fillMessagePush(resp, data);
        assert(resp.data["message_id"].get<std::string>() == "msg_2");
        assert(resp.data["conversation_id"].get<std::string>() == "conv_2");
        assert(resp.data["from_user_id"].get<UserId>() == 111);
        assert(resp.data["from_username"].get<std::string>() == "alice");
        assert(resp.data["to_user_id"].get<UserId>() == 222);
        assert(resp.data["content"].get<std::string>() == "hi");
        assert(resp.data["created_at"].get<Timestamp>() == 1621948800);
        assert(resp.data["server_time"].get<Timestamp>() == 1621948800);
    }
}

void TestPullOfflineMessages() {
    JsonCodec codec;
    std::string err;

    // 1. Success case with optional fields
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.seq = 200;
        msg.data = nlohmann::json::object();
        msg.data["limit"] = 20;
        msg.data["before_message_id"] = "msg_before";
        msg.data["since_message_id"] = "msg_since";

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(ok);
        assert(req.limit == 20);
        assert(req.before_message_id == "msg_before");
        assert(req.since_message_id == "msg_since");
    }

    // 2. Success case without optional fields
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.seq = 200;
        msg.data = nlohmann::json::object();
        msg.data["limit"] = 10;

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(ok);
        assert(req.limit == 10);
        assert(req.before_message_id.empty());
        assert(req.since_message_id.empty());
    }

    // 3. Missing limit
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.data = nlohmann::json::object();

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(!ok);
        assert(err.find("limit") != std::string::npos);
    }

    // 4. Invalid limit (not integer)
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.data = nlohmann::json::object();
        msg.data["limit"] = "10";

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(!ok);
        assert(err.find("limit") != std::string::npos);
    }

    // 5. Invalid limit value (<= 0)
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.data = nlohmann::json::object();
        msg.data["limit"] = 0;

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(!ok);
        assert(err.find("limit") != std::string::npos);
    }

    // 6. Invalid limit value (> 100)
    {
        Message msg;
        msg.msg_type = "pull_offline_messages";
        msg.data = nlohmann::json::object();
        msg.data["limit"] = 101;

        PullOfflineMessagesRequest req;
        bool ok = codec.parsePullOfflineMessagesRequest(msg, req, err);
        assert(!ok);
        assert(err.find("limit") != std::string::npos);
    }

    // 7. fillPullOfflineMessagesResponse
    {
        Response resp;
        PullOfflineMessagesResponseData data;
        data.has_more = true;

        OfflineMessage msg1;
        msg1.message_id = "m1";
        msg1.conversation_id = "c1";
        msg1.from_user_id = 111;
        msg1.to_user_id = 222;
        msg1.content = "hello";
        msg1.created_at = 12345;
        msg1.status = 1;
        data.messages.push_back(msg1);

        codec.fillPullOfflineMessagesResponse(resp, data);
        assert(resp.data["has_more"].get<bool>() == true);
        assert(resp.data["messages"].is_array());
        assert(resp.data["messages"].size() == 1);
        assert(resp.data["messages"][0]["message_id"].get<std::string>() == "m1");
        assert(resp.data["messages"][0]["conversation_id"].get<std::string>() == "c1");
        assert(resp.data["messages"][0]["from_user_id"].get<UserId>() == 111);
        assert(resp.data["messages"][0]["to_user_id"].get<UserId>() == 222);
        assert(resp.data["messages"][0]["content"].get<std::string>() == "hello");
        assert(resp.data["messages"][0]["created_at"].get<Timestamp>() == 12345);
        assert(resp.data["messages"][0]["status"].get<int32_t>() == 1);
    }
}

}  // namespace

int main() {
    TestDecodeAndParseLoginRequest();
    TestRejectInvalidJson();
    TestRejectMissingRequiredField();
    TestRejectInvalidTokenType();
    TestEncodeResponseReturnsPureJson();
    TestSendMessageRequest();
    TestPullOfflineMessages();
    std::cout << "[PASS] json codec tests passed\n";
    return 0;
}
