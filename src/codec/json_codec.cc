#include "codec/json_codec.h"

namespace chat
{
    bool JsonCodec::decodeMessage(const std::string &raw, Message &msg,
                                  std::string &err) const
    {
        try
        {
            // 尝试解析json对象
            auto json_obj = nlohmann::json::parse(raw);

            // 校验协议信封的必填字段及其类型
            if (!json_obj.contains("msg_type") || !json_obj["msg_type"].is_string())
            {
                err = "Missing or invalid 'msg_type' field";
                return false;
            }

            if (!json_obj.contains("seq") || !json_obj["seq"].is_number_integer())
            {
                err = "Missing or invalid 'seq' field";
                return false;
            }

            if (!json_obj.contains("data") || !json_obj["data"].is_object())
            {
                err = "Missing or invalid 'data' field";
                return false;
            }

            // 填充消息对象
            msg.msg_type = json_obj["msg_type"].get<std::string>();
            msg.seq = json_obj["seq"].get<SeqId>();
            msg.data = json_obj["data"];

            // token字段允许缺失或为空字符串
            if (json_obj.contains("token"))
            {
                if (!json_obj["token"].is_string())
                {
                    msg.token = "";
                }
                else
                {
                    msg.token = json_obj["token"].get<std::string>();
                }
            }
            return true;
        }
        catch (const nlohmann::json::exception &e)
        {
            // 捕获parse error 或者其他json异常
            err = std::string("JSON parsing error: ") + e.what();
            return false;
        }
    }
    std::string JsonCodec::encodeResponse(const Response &resp) const
    {
        nlohmann::json json_obj;
        json_obj["msg_type"] = resp.msg_type;
        json_obj["seq"] = resp.seq;

        // 枚举类型需要转换为整数
        json_obj["code"] = static_cast<int>(resp.code);
        json_obj["message"] = resp.message;

        // data字段允许为null，但不能缺失
        if (resp.data.is_null())
        {
            json_obj["data"] = nlohmann::json::object();
        }
        else
        {
            json_obj["data"] = resp.data;
        }

        return json_obj.dump();
    }

    bool JsonCodec::parseRegisterRequest(const Message &msg, RegisterRequest &req,
                                         std::string &err) const
    {
        if (!msg.data.contains("username") || !msg.data["username"].is_string())
        {
            err = "Missing or invalid 'username' field in data";
            return false;
        }
        if (!msg.data.contains("password") || !msg.data["password"].is_string())
        {
            err = "Missing or invalid 'password' field in data";
            return false;
        }

        req.username = msg.data["username"].get<std::string>();
        req.password = msg.data["password"].get<std::string>();

        // nickname字段可选，如果存在必须是字符串
        if (msg.data.contains("nickname") && msg.data["nickname"].is_string())
        {
            req.nickname = msg.data["nickname"].get<std::string>();
        }
        else
        {
            req.nickname = "";
        }
        return true;
    }

    bool JsonCodec::parseLoginRequest(const Message &msg, LoginRequest &req,
                                      std::string &err) const
    {
        if (!msg.data.contains("username") || !msg.data["username"].is_string())
        {
            err = "Missing or invalid 'username' field in data";
            return false;
        }
        if (!msg.data.contains("password") || !msg.data["password"].is_string())
        {
            err = "Missing or invalid 'password' field in data";
            return false;
        }

        req.username = msg.data["username"].get<std::string>();
        req.password = msg.data["password"].get<std::string>();
        return true;
    }

    void JsonCodec::fillRegisterResponse(Response &resp,
                                         const RegisterResponseData &data) const
    {
        resp.data["user_id"] = data.user_id;
    }

    void JsonCodec::fillLoginResponse(Response &resp,
                                      const LoginResponseData &data) const
    {
        resp.data["user_id"] = data.user_id;
        resp.data["nickname"] = data.nickname;
        resp.data["token"] = data.token;
    }

    void JsonCodec::fillHeartbeatResponse(Response &resp,
                                          const HeartbeatResponseData &data) const
    {
        resp.data["server_time"] = data.server_time;
    }
} // namespace chat