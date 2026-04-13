#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_AUTH_MESSAGES_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_AUTH_MESSAGES_H_

#include <string>

#include "common/types.h"

// 本文件定义认证相关协议消息的业务字段模型。
// 这些结构体只描述 data 字段内容，不包含通用信封字段。
//
// TODO(lzq): 为每个请求结构补充字段合法性约束说明。
// TODO(lzq): 增加刷新 token、修改密码等认证扩展消息。
// TODO(lzq): 与 README 中的 JSON 示例保持字段命名完全一致。

namespace chat {

// 注册请求中的业务字段。
struct RegisterRequest {
  // 待注册的用户名。
  std::string username;

  // 客户端提交的原始密码。
  std::string password;

  // 用户昵称，可选。
  std::string nickname;
};

// 注册成功后返回给客户端的数据。
struct RegisterResponseData {
  // 新创建用户的 ID。
  UserId user_id = 0;
};

// 登录请求中的业务字段。
struct LoginRequest {
  // 待登录的用户名。
  std::string username;

  // 客户端提交的原始密码。
  std::string password;
};

// 登录成功后返回给客户端的数据。
struct LoginResponseData {
  // 已登录用户的 ID。
  UserId user_id = 0;

  // 当前用户昵称。
  std::string nickname;

  // 服务端下发的认证令牌。
  std::string token;
};

// 登出请求的数据体；当前版本无额外字段。
struct LogoutRequest {};

// 心跳请求的数据体；当前版本无额外字段。
struct HeartbeatRequest {};

// 心跳响应中返回的数据。
struct HeartbeatResponseData {
  // 服务端当前时间戳。
  Timestamp server_time = 0;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_AUTH_MESSAGES_H_
