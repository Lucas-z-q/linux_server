#ifndef LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
#define LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_

#include <memory>
#include <string>
#include <vector>

#include "common/types.h"
#include "model/connection_session.h"
#include "net/ConnectionContext.h"
#include "net/IMessageHandler.h"

// 表示 worker 处理完成后等待回到 I/O 线程发送的响应任务。
struct ResponseTask {
    std::weak_ptr<ConnectionContext> context;
    chat::ConnectionId conn_id;
    std::string response;
    uint64_t sequence;

    // I/O级副作用
    bool close_after;

    // 业务/会话级副作用
    SessionAction session_action = SessionAction::NONE;
    chat::ConnectionSession pending_session;

    // 待回到 I/O 线程分发的主动推送消息列表。
    std::vector<OutboundMessage> pushes;

    // 响应成功入队后需要标记为 delivered 的消息 ID 列表（离线拉取场景）。
    std::vector<std::string> delivered_message_ids;

    // delivered_message_ids 对应的接收方用户 ID。
    chat::UserId delivered_user_id = 0;
};

#endif  // LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
