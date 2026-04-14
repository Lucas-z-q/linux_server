#ifndef LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_
#define LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_

#include <string>

#include "codec/json_codec.h"
#include "common/message.h"
#include "common/response.h"
#include "net/IMessageHandler.h"
#include "service/user_service.h"

// 本文件声明统一消息处理器。
// 该处理器位于网络层与业务层之间，负责请求解析、路由和响应封装。
//
// TODO(lzq): 根据消息类型继续拆分认证、聊天、好友等子处理器。
// TODO(lzq): 为每种消息类型补充结构化日志与耗时统计。
// TODO(lzq): 当引入业务线程池后，评估是否改成异步处理模型。

namespace chat
{

  // 负责处理一条原始请求并输出一条原始响应。
  class MessageHandler : public IMessageHandler
  {
  public:
    MessageHandler() = default;
    ~MessageHandler() override = default;

    // 处理客户端发送的原始请求字符串，并返回编码后的响应字符串。
    std::string handle(const std::string &raw_request) override;

  private:
    // 处理注册请求。
    Response handleRegister(const Message &msg);

    // 处理登录请求。
    Response handleLogin(const Message &msg);

    // 处理登出请求。
    Response handleLogout(const Message &msg);

    // 处理心跳请求。
    Response handleHeartbeat(const Message &msg);

    // 处理未知消息类型的请求。
    Response handleUnknown(const Message &msg);

    // 负责 JSON 编解码的组件。
    JsonCodec codec_;

    // 负责用户注册、登录等业务逻辑的组件。
    UserService user_service_;
  };

} // namespace chat

#endif // LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_
