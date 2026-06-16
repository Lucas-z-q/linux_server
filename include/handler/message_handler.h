#ifndef LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_
#define LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_

#include <string>

#include "codec/json_codec.h"
#include "common/message.h"
#include "common/response.h"
#include "net/IMessageHandler.h"
#include "service/chat_service.h"
#include "service/user_service.h"

// 本文件声明统一消息处理器。
// 该处理器位于网络层与业务层之间，负责请求解析、路由和响应封装。
//
// TODO(lzq): 根据消息类型继续拆分认证、聊天、好友等子处理器。
// TODO(lzq): 为每种消息类型补充结构化日志与耗时统计。
// TODO(lzq): 当引入业务线程池后，评估是否改成异步处理模型。

namespace chat {

// 负责处理一条原始请求并输出一条原始响应。
class MessageHandler : public IMessageHandler {
   public:
    explicit MessageHandler(UserService &user_service, ChatService &chat_service)
        : user_service_(user_service), chat_service_(chat_service) {}
    ~MessageHandler() override = default;

    // 处理客户端发送的原始请求字符串，并返回HandleResult结构体。
    HandleResult handle(const std::string &raw_request, const RequestContext &context) override;

    // 单元测试和进程内调用不关心 IP 时使用此便捷入口。
    HandleResult handle(const std::string &raw_request, chat::ConnectionId conn_id) {
        return handle(raw_request, RequestContext{conn_id, ""});
    }

    // 连接关闭后清理该连接绑定的登录态。
    void onConnectionClosed(chat::ConnectionId conn_id) override;

    void applyBindSession(chat::ConnectionId conn_id, const chat::ConnectionSession &session) override {
        user_service_.bindSession(conn_id, session);
    }

    void applyUnbindSession(chat::ConnectionId conn_id) override { user_service_.logoutSession(conn_id); }

    bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) override {
        return user_service_.isConnectionBoundToUser(conn_id, user_id);
    }

    void onMessagesDelivered(chat::UserId user_id, const std::vector<std::string> &message_ids) override;

   private:
    // 处理注册请求。
    HandleResult handleRegister(const Message &msg, const std::string &identity);

    // 处理登录请求。
    HandleResult handleLogin(const Message &msg, chat::ConnectionId conn_id, const std::string &identity);

    HandleResult handleResumeSession(const Message &msg);

    // 处理登出请求。
    HandleResult handleLogout(const Message &msg, chat::ConnectionId conn_id);

    // 查询当前连接绑定的登录态。
    HandleResult handleWhoAmI(const Message &msg, chat::ConnectionId conn_id);

    // 处理心跳请求。
    HandleResult handleHeartbeat(const Message &msg);

    // 处理发送消息请求。
    HandleResult handleSendMessage(const Message &msg, chat::ConnectionId conn_id);

    // 处理拉取离线消息请求。
    HandleResult handlePullOfflineMessages(const Message &msg, chat::ConnectionId conn_id);

    // 处理未知消息类型的请求。
    HandleResult handleUnknown(const Message &msg);

    // 负责 JSON 编解码的组件。
    JsonCodec codec_;

    // 负责用户注册、登录等业务逻辑的组件。
    UserService &user_service_;

    // 负责单聊等即时通讯核心业务逻辑的组件。
    ChatService &chat_service_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_HANDLER_MESSAGE_HANDLER_H_
