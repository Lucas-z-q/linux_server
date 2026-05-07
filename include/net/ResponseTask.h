#ifndef LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
#define LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_

#include <memory>
#include <string>

#include "common/types.h"
#include "net/ConnectionContext.h"

// 表示 worker 处理完成后等待回到 I/O 线程发送的响应任务。
struct ResponseTask {
    std::weak_ptr<ConnectionContext> context;
    chat::ConnectionId conn_id;
    std::string response;
    uint64_t sequence;
    bool close_after;
};

#endif  // LINUX_SERVER_INCLUDE_NET_RESPONSE_TASK_H_
