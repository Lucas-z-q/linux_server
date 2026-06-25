#ifndef LINUX_SERVER_INCLUDE_COMMON_TYPES_H_
#define LINUX_SERVER_INCLUDE_COMMON_TYPES_H_

#include <cstdint>

// 本文件定义聊天服务器项目中的基础类型别名。
// 这些类型被协议层、业务层、数据层和网络层共同依赖，
// 因此应尽量保持稳定、轻量，并避免引入额外依赖。

namespace chat {

// 表示业务层用户唯一标识。
using UserId = std::int64_t;

// 表示连接层使用的轻量连接标识。
using ConnectionId = std::uint64_t;

// 表示一条请求与响应配对使用的消息序号。
using SeqId = std::uint64_t;

// 表示协议中传输的时间戳，当前约定为整数时间值。
using Timestamp = std::int64_t;

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_COMMON_TYPES_H_
