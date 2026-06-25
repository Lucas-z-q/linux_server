#ifndef LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_
#define LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_

#include <string>
#include <vector>

#include "common/types.h"
#include "model/connection_session.h"

// 本文件定义网络层与业务处理层之间的最小接口。
// TcpServer 只依赖这个抽象，从而避免与具体业务实现耦合。
//
// 【并发与生命周期语义】
// 1. handle() 在 worker 线程执行，可以调用持久化和外部服务，但不能直接操作 socket。
// 2. 本地连接会话的绑定和解绑通过 HandleResult 延后到 I/O 线程执行。
// 3. TcpServer 在应用延后副作用前重新校验连接是否存活；失效连接的响应和会话动作会被丢弃。
// 4. handle() 抛出的异常由 TcpServer 捕获，并转换为关闭连接的响应任务。

enum class SessionAction { NONE, BIND, UNBIND };

struct RequestContext {
    chat::ConnectionId conn_id = 0;
    std::string peer_ip;
};

// 表示发往特定连接的推送消息负载。
struct OutboundMessage {
    // 目标客户端的连接 ID。
    chat::ConnectionId target_conn_id = 0;

    // 目标接收方用户 ID（用于分发前的身份校验）。
    chat::UserId target_user_id = 0;

    // 待推送的原始消息数据（通常为序列化后的 JSON 字符串）。
    std::string payload;

    // 推送成功进入目标连接发送队列后需要标记为 delivered 的消息 ID。
    std::string message_id;
};

struct HandleResult {
    // 投递回发送方客户端的即时响应。
    std::string response;

    // 需要推送给其他客户端的异步/主动推送消息列表。
    std::vector<OutboundMessage> pushes;

    SessionAction session_action = SessionAction::NONE;
    chat::ConnectionSession pending_session;  // 仅在bind时有效

    // 响应成功入队后需要在 I/O 线程标记为 delivered 的消息 ID 列表（离线拉取场景）。
    std::vector<std::string> delivered_message_ids;

    // delivered_message_ids 对应的接收方用户 ID，用于 markDelivered 的所有权校验。
    chat::UserId delivered_user_id = 0;
};
// 抽象一类“输入请求字符串，输出响应字符串”的消息处理器。
class IMessageHandler {
   public:
    // 允许通过基类指针安全析构派生类。
    virtual ~IMessageHandler() = default;

    // 处理一条请求报文，并返回要发送给客户端的响应报文。
    // 注意：该方法在 Worker 线程执行，不得直接操作 Session。
    virtual HandleResult handle(const std::string &request, const RequestContext &context) = 0;

    // 连接关闭后清理业务状态。TcpServer 将该回调投递到 worker pool，
    // 避免 Redis 或数据库清理阻塞 I/O 线程。
    virtual void onConnectionClosed(chat::ConnectionId conn_id) { (void)conn_id; }

    // 以下方法供 I/O 线程在回投任务 lock() 成功后调用，真正应用延后的副作用。
    virtual void applyBindSession(chat::ConnectionId conn_id, const chat::ConnectionSession &session) {}
    virtual void applyUnbindSession(chat::ConnectionId conn_id) {}

    // 检查指定连接当前是否仍属于目标用户（用于推送安全性校验）。
    // 此方法为纯虚函数，强制所有消息处理器显式声明并实现安全性校验策略。
    virtual bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) = 0;

    // I/O 线程在响应/推送成功进入发送队列后调用，标记消息已投递。
    // delivered 表示服务端已投递/已返回给客户端，不等价于客户端已读。
    // 默认空实现，需要该能力的 handler 自行覆盖。
    virtual void onMessagesDelivered(chat::UserId user_id, const std::vector<std::string> &message_ids) {
        (void)user_id;
        (void)message_ids;
    }
};

#endif  // LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_
