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
// 1. Worker 线程通过 handle() 处理请求，此过程为纯粹的查库计算，不允许直接修改全局会话状态。
// 2. Worker 产出 HandleResult (含响应报文及可延后执行的 SessionAction) 投递回 I/O 线程。
// 3. I/O 线程接手时需通过 weak_ptr<Connection>.lock() 校验连接存活状态：
//    - 若存活：发送响应，并根据 action 调用 applyBindSession / applyUnbindSession 落实副作用。
//    - 若失效：直接丢弃 HandleResult 及副作用。不再尝试取消 worker 任务。
//
// TODO(lzq): 在接口中加入连接上下文参数，支持按连接处理认证态。
// TODO(lzq): 明确 handle 的异常约束，统一使用返回值还是错误对象。

enum class SessionAction { NONE, BIND, UNBIND };

// 表示发往特定连接的推送消息负载。
struct OutboundMessage {
    // 目标客户端的连接 ID。
    chat::ConnectionId target_conn_id = 0;

    // 目标接收方用户 ID（用于分发前的身份校验）。
    chat::UserId target_user_id = 0;

    // 待推送的原始消息数据（通常为序列化后的 JSON 字符串）。
    std::string payload;
};

struct HandleResult {
    // 投递回发送方客户端的即时响应。
    std::string response;

    // 需要推送给其他客户端的异步/主动推送消息列表。
    std::vector<OutboundMessage> pushes;

    SessionAction session_action = SessionAction::NONE;
    chat::ConnectionSession pending_session;  // 仅在bind时有效
};
// 抽象一类“输入请求字符串，输出响应字符串”的消息处理器。
class IMessageHandler {
   public:
    // 允许通过基类指针安全析构派生类。
    virtual ~IMessageHandler() = default;

    // 处理一条请求报文，并返回要发送给客户端的响应报文。
    // 注意：该方法在 Worker 线程执行，不得直接操作 Session。
    virtual HandleResult handle(const std::string &request, chat::ConnectionId conn_id) = 0;

    // 连接关闭后通知业务层清理与该连接绑定的状态。
    // 该方法仅在 I/O 线程中由网络层 (如 closeClientFd) 触发。
    virtual void onConnectionClosed(chat::ConnectionId conn_id) { (void)conn_id; }

    // 以下方法供 I/O 线程在回投任务 lock() 成功后调用，真正应用延后的副作用。
    virtual void applyBindSession(chat::ConnectionId conn_id, const chat::ConnectionSession &session) {}
    virtual void applyUnbindSession(chat::ConnectionId conn_id) {}

    // 检查指定连接当前是否仍属于目标用户（用于推送安全性校验）。
    virtual bool isConnectionBoundToUser(chat::ConnectionId conn_id, chat::UserId user_id) {
        (void)conn_id;
        (void)user_id;
        return true;
    }
};

#endif  // LINUX_SERVER_INCLUDE_NET_IMESSAGE_HANDLER_H_
