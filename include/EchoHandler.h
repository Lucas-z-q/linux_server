#ifndef LINUX_SERVER_INCLUDE_ECHO_HANDLER_H_
#define LINUX_SERVER_INCLUDE_ECHO_HANDLER_H_

#include <string>

#include "net/IMessageHandler.h"

// 本文件声明一个最简单的回显示例处理器。
// 它适合在引入注册、登录协议之前用于验证网络收发链路是否打通。
//
// TODO(lzq): 用新的业务 MessageHandler 替换当前 EchoHandler。
// TODO(lzq): 为示例处理器补充简单的连通性测试用例。
// TODO(lzq): 统一顶层 include 路径，避免后续 net 目录迁移时产生歧义。

// 一个将请求原样返回的消息处理器实现。
class EchoHandler : public IMessageHandler {
   public:
    // 处理一条请求，并直接返回相同内容作为响应。
    HandleResult handle(const std::string& request, chat::ConnectionId conn_id) override;

    // EchoHandler 不涉及用户会话，为测试简便起见直接允许推送通过（默认返回 true）。
    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override;
};
#endif  // LINUX_SERVER_INCLUDE_ECHO_HANDLER_H_
