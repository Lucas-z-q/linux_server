#ifndef LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_
#define LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_

#include <string>

#include "packet_codec.h"
#include "common/message.h"
#include "common/response.h"
#include "protocol/auth_messages.h"

// 本文件声明 JSON 编解码器。
// 该类负责在原始字符串与协议对象之间转换，不承担业务校验职责。
//
// TODO(lzq): 为 decodeMessage 增加更细粒度的字段校验错误信息。
// TODO(lzq): 为 auth 之外的聊天消息补充解析与填充函数。
// TODO(lzq): 统一编码时的字段输出顺序，便于测试断言。

namespace chat
{

    // 负责处理协议对象与 JSON 字符串之间的编解码转换。
    class JsonCodec
    {
    public:
        // 解析一段原始 JSON 字符串到通用消息对象。
        // 解析失败时返回 false，并通过 err 输出原因。
        bool decodeMessage(const std::string &raw, Message &msg,
                           std::string &err) const;

        // 将通用响应对象编码为 JSON 字符串。
        std::string encodeResponse(const Response &resp) const;

        // 从通用消息中提取注册请求结构。
        bool parseRegisterRequest(const Message &msg, RegisterRequest &req,
                                  std::string &err) const;

        // 从通用消息中提取登录请求结构。
        bool parseLoginRequest(const Message &msg, LoginRequest &req,
                               std::string &err) const;

        // 将注册响应业务数据写回通用响应对象。
        void fillRegisterResponse(Response &resp,
                                  const RegisterResponseData &data) const;

        // 将登录响应业务数据写回通用响应对象。
        void fillLoginResponse(Response &resp,
                               const LoginResponseData &data) const;

        // 将心跳响应业务数据写回通用响应对象。
        void fillHeartbeatResponse(Response &resp,
                                   const HeartbeatResponseData &data) const;
    };

} // namespace chat

#endif // LINUX_SERVER_INCLUDE_CODEC_JSON_CODEC_H_
