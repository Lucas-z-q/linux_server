#ifndef LINUX_SERVER_INCLUDE_MODEL_CONNECTION_SESSION_H_
#define LINUX_SERVER_INCLUDE_MODEL_CONNECTION_SESSION_H_

#include <string>

#include "common/types.h"

// 本文件定义连接与用户身份绑定后的会话模型。
// 它主要服务于 TCP 长连接场景下的鉴权与在线状态维护。
//
// TODO(lzq): 增加最近活跃时间和登录时间字段。
// TODO(lzq): 明确 token 是否允许一个连接对应多个会话。
// TODO(lzq): 为多端登录场景补充设备标识字段。

namespace chat {

// 表示一个连接当前对应的登录态信息。
struct ConnectionSession {
    // 当前连接是否已经完成认证。
    bool authenticated = false;

    // 当前连接绑定的用户 ID。
    UserId user_id = 0;

    // 当前连接绑定的用户名。
    std::string username;

    // 当前连接最近一次登录生成的令牌。
    std::string token;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_MODEL_CONNECTION_SESSION_H_
