#include "codec/json_codec.h"

#include <cassert>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "common/message.h"
#include "common/response.h"

using namespace chat;

namespace
{

void TestDecodeAndParseLoginRequest()
{
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

void TestRejectInvalidJson()
{
    JsonCodec codec;
    const std::string bad_json =
        R"({"msg_type":"login", "seq": 3, "data": { broken...)";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(bad_json, msg, err);
    assert(!decode_ok);
}

void TestRejectMissingRequiredField()
{
    JsonCodec codec;
    const std::string missing_field_json =
        R"({"msg_type":"login","seq":4,"data":{"username":"alice"}})";
    Message msg;
    std::string err;

    const bool decode_ok = codec.decodeMessage(missing_field_json, msg, err);
    assert(decode_ok);

    LoginRequest req;
    const bool parse_ok = codec.parseLoginRequest(msg, req, err);
    assert(!parse_ok);
}

void TestEncodeResponseReturnsPureJson()
{
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

void TestSendMessageRequest()
{
    JsonCodec codec;
    std::string err;

    // 1. Success case
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.seq = 100;
        msg.data = nlohmann::json::object();
        msg.data["to_user_id"] = 12345;
        msg.data["content"] = "hello world";

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(ok);
        assert(req.receiver_id == 12345);
        assert(req.content == "hello world");
    }

    // 2. Missing to_user_id
    {
        Message msg;
        msg.msg_type = "send_message";
        msg.data = nlohmann::json::object();
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
        msg.data["to_user_id"] = 123;
        msg.data["content"] = std::string(4097, 'a');

        SendMessageRequest req;
        bool ok = codec.parseSendMessageRequest(msg, req, err);
        assert(!ok);
        assert(err.find("too long") != std::string::npos);
    }

    // 9. fillSendMessageAck
    {
        Response resp;
        SendMessageAckData data;
        data.receiver_id = 999;

        codec.fillSendMessageAck(resp, data);
        assert(resp.data["to_user_id"].get<UserId>() == 999);
    }

    // 10. fillMessagePush
    {
        Response resp;
        MessagePushData data;
        data.from_user_id = 111;
        data.from_username = "alice";
        data.content = "hi";
        data.server_time = 1621948800;

        codec.fillMessagePush(resp, data);
        assert(resp.data["from_user_id"].get<UserId>() == 111);
        assert(resp.data["from_username"].get<std::string>() == "alice");
        assert(resp.data["content"].get<std::string>() == "hi");
        assert(resp.data["server_time"].get<Timestamp>() == 1621948800);
    }
}

}  // namespace

int main()
{
    TestDecodeAndParseLoginRequest();
    TestRejectInvalidJson();
    TestRejectMissingRequiredField();
    TestEncodeResponseReturnsPureJson();
    TestSendMessageRequest();
    std::cout << "[PASS] json codec tests passed\n";
    return 0;
}
