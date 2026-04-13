#ifndef LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_
#define LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_

// 本文件预留会话管理器接口位置。
// 后续将用于维护 user_id、token 与连接之间的映射关系。
//
// TODO(lzq): 定义绑定登录态、查询会话和清理会话接口。
// TODO(lzq): 增加单点登录与多端登录策略设计。
// TODO(lzq): 评估是否需要将会话状态持久化到 Redis。

namespace chat {

// 预留的会话管理器占位类型。
class SessionManager {};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVER_SESSION_MANAGER_H_
