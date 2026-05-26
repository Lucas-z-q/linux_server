#include "codec/json_codec.h"

namespace chat {
bool JsonCodec::decodeMessage(const std::string &raw, Message &msg, std::string &err) const {
    try {
        // 尝试解析json对象
        auto json_obj = nlohmann::json::parse(raw);

        // 校验协议信封的必填字段及其类型
        if (!json_obj.contains("msg_type") || !json_obj["msg_type"].is_string()) {
            err = "Missing or invalid 'msg_type' field";
            return false;
        }

        if (!json_obj.contains("seq") || !json_obj["seq"].is_number_integer()) {
            err = "Missing or invalid 'seq' field";
            return false;
        }

        if (!json_obj.contains("data") || !json_obj["data"].is_object()) {
            err = "Missing or invalid 'data' field";
            return false;
        }

        // 填充消息对象
        msg.msg_type = json_obj["msg_type"].get<std::string>();
        msg.seq = json_obj["seq"].get<SeqId>();
        msg.data = json_obj["data"];

        // token字段允许缺失或为空字符串
        if (json_obj.contains("token")) {
            if (!json_obj["token"].is_string()) {
                msg.token = "";
            } else {
                msg.token = json_obj["token"].get<std::string>();
            }
        }
        return true;
    } catch (const nlohmann::json::exception &e) {
        // 捕获parse error 或者其他json异常
        err = std::string("JSON parsing error: ") + e.what();
        return false;
    }
}
std::string JsonCodec::encodeResponse(const Response &resp) const {
    nlohmann::json json_obj;
    json_obj["msg_type"] = resp.msg_type;
    json_obj["seq"] = resp.seq;

    // 枚举类型需要转换为整数
    json_obj["code"] = static_cast<int>(resp.code);
    json_obj["message"] = resp.message;

    // data字段允许为null，但不能缺失
    if (resp.data.is_null()) {
        json_obj["data"] = nlohmann::json::object();
    } else {
        json_obj["data"] = resp.data;
    }

    return json_obj.dump();
}

bool JsonCodec::parseRegisterRequest(const Message &msg, RegisterRequest &req, std::string &err) const {
    if (!msg.data.contains("username") || !msg.data["username"].is_string()) {
        err = "Missing or invalid 'username' field in data";
        return false;
    }
    if (!msg.data.contains("password") || !msg.data["password"].is_string()) {
        err = "Missing or invalid 'password' field in data";
        return false;
    }

    req.username = msg.data["username"].get<std::string>();
    req.password = msg.data["password"].get<std::string>();

    // nickname字段可选，如果存在必须是字符串
    if (msg.data.contains("nickname") && msg.data["nickname"].is_string()) {
        req.nickname = msg.data["nickname"].get<std::string>();
    } else {
        req.nickname = "";
    }
    return true;
}

bool JsonCodec::parseLoginRequest(const Message &msg, LoginRequest &req, std::string &err) const {
    if (!msg.data.contains("username") || !msg.data["username"].is_string()) {
        err = "Missing or invalid 'username' field in data";
        return false;
    }
    if (!msg.data.contains("password") || !msg.data["password"].is_string()) {
        err = "Missing or invalid 'password' field in data";
        return false;
    }

    req.username = msg.data["username"].get<std::string>();
    req.password = msg.data["password"].get<std::string>();
    return true;
}

void JsonCodec::fillRegisterResponse(Response &resp, const RegisterResponseData &data) const {
    resp.data["user_id"] = data.user_id;
}

void JsonCodec::fillLoginResponse(Response &resp, const LoginResponseData &data) const {
    resp.data["user_id"] = data.user_id;
    resp.data["nickname"] = data.nickname;
    resp.data["token"] = data.token;
}

void JsonCodec::fillHeartbeatResponse(Response &resp, const HeartbeatResponseData &data) const {
    resp.data["server_time"] = data.server_time;
}

bool JsonCodec::parseSendMessageRequest(const Message &msg, SendMessageRequest &req, std::string &err) const {
    if (!msg.data.contains("client_msg_id")) {
        err = "Missing 'client_msg_id' field in data";
        return false;
    }
    if (!msg.data["client_msg_id"].is_string()) {
        err = "Invalid 'client_msg_id' field in data: must be a string";
        return false;
    }
    std::string client_msg_id = msg.data["client_msg_id"].get<std::string>();
    if (client_msg_id.empty()) {
        err = "Empty 'client_msg_id' field in data";
        return false;
    }
    if (client_msg_id.length() > 64) {
        err = "'client_msg_id' field in data is too long (max 64)";
        return false;
    }

    if (!msg.data.contains("to_user_id")) {
        err = "Missing 'to_user_id' field in data";
        return false;
    }
    if (!msg.data["to_user_id"].is_number_integer()) {
        err = "Invalid 'to_user_id' field in data: must be an integer";
        return false;
    }
    UserId to_user_id = msg.data["to_user_id"].get<UserId>();
    if (to_user_id <= 0) {
        err = "Invalid 'to_user_id' field in data: must be greater than 0";
        return false;
    }

    if (!msg.data.contains("content")) {
        err = "Missing 'content' field in data";
        return false;
    }
    if (!msg.data["content"].is_string()) {
        err = "Invalid 'content' field in data: must be a string";
        return false;
    }

    std::string content = msg.data["content"].get<std::string>();
    if (content.empty()) {
        err = "Empty 'content' field in data";
        return false;
    }

    if (content.length() > 4096) {
        err = "'content' field in data is too long";
        return false;
    }

    req.client_msg_id = std::move(client_msg_id);
    req.to_user_id = to_user_id;
    req.content = std::move(content);
    return true;
}

void JsonCodec::fillSendMessageAck(Response &resp, const SendMessageAckData &data) const {
    resp.data["message_id"] = data.message_id;
    resp.data["conversation_id"] = data.conversation_id;
    resp.data["to_user_id"] = data.to_user_id;
    resp.data["status"] = data.status;
    resp.data["created_at"] = data.created_at;
}

void JsonCodec::fillMessagePush(Response &resp, const MessagePushData &data) const {
    resp.data["message_id"] = data.message_id;
    resp.data["conversation_id"] = data.conversation_id;
    resp.data["from_user_id"] = data.from_user_id;
    resp.data["from_username"] = data.from_username;
    resp.data["to_user_id"] = data.to_user_id;
    resp.data["content"] = data.content;
    resp.data["created_at"] = data.created_at;
    resp.data["server_time"] = data.server_time;
}

bool JsonCodec::parsePullOfflineMessagesRequest(const Message &msg, PullOfflineMessagesRequest &req, std::string &err) const {
    if (!msg.data.contains("limit")) {
        err = "Missing 'limit' field in data";
        return false;
    }
    if (!msg.data["limit"].is_number_integer()) {
        err = "Invalid 'limit' field in data: must be an integer";
        return false;
    }
    int32_t limit = msg.data["limit"].get<int32_t>();
    if (limit <= 0) {
        err = "Invalid 'limit' field in data: must be greater than 0";
        return false;
    }
    req.limit = limit;

    if (msg.data.contains("before_message_id")) {
        if (!msg.data["before_message_id"].is_string()) {
            err = "Invalid 'before_message_id' field in data: must be a string";
            return false;
        }
        req.before_message_id = msg.data["before_message_id"].get<std::string>();
    } else {
        req.before_message_id = "";
    }

    if (msg.data.contains("since_message_id")) {
        if (!msg.data["since_message_id"].is_string()) {
            err = "Invalid 'since_message_id' field in data: must be a string";
            return false;
        }
        req.since_message_id = msg.data["since_message_id"].get<std::string>();
    } else {
        req.since_message_id = "";
    }

    return true;
}

void JsonCodec::fillPullOfflineMessagesResponse(Response &resp, const PullOfflineMessagesResponseData &data) const {
    nlohmann::json list_arr = nlohmann::json::array();
    for (const auto &m : data.messages) {
        nlohmann::json item;
        item["message_id"] = m.message_id;
        item["conversation_id"] = m.conversation_id;
        item["from_user_id"] = m.from_user_id;
        item["to_user_id"] = m.to_user_id;
        item["content"] = m.content;
        item["created_at"] = m.created_at;
        item["status"] = m.status;
        list_arr.push_back(item);
    }
    resp.data["messages"] = list_arr;
    resp.data["has_more"] = data.has_more;
}
}  // namespace chat