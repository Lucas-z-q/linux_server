#include "codec/json_codec.h"

#include <cassert>
#include <iostream>
#include <string>

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

void TestEncodeResponseAppendsNewline()
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
    assert(out_json.back() == '\n');
}

}  // namespace

int main()
{
    TestDecodeAndParseLoginRequest();
    TestRejectInvalidJson();
    TestRejectMissingRequiredField();
    TestEncodeResponseAppendsNewline();
    std::cout << "[PASS] json codec tests passed\n";
    return 0;
}
