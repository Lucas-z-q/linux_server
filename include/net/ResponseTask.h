#ifndef LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
#define LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_

#include <memory>
#include <string>

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
};

#endif  // LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
