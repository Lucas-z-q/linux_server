#ifndef LINUX_SERVER_INCLUDE_COMMON_MESSAGE_H_
#define LINUX_SERVER_INCLUDE_COMMON_MESSAGE_H_

#include <string>

#include <nlohmann/json.hpp>

#include "common/types.h"

// 本文件定义客户端请求在服务端内部使用的通用消息信封。
// 顶层字段尽量保持统一，具体业务参数放入 data 字段中。
//
// TODO(lzq): 为 Message 增加来源连接信息，便于业务层鉴权。
// TODO(lzq): 明确 token 字段在连接态认证和令牌认证中的职责边界。
// TODO(lzq): 评估是否需要为 data 增加 schema 校验入口。

namespace chat {

// 表示一条从客户端发往服务端的通用协议消息。
struct Message {
  // 指定消息类型，例如 login、register、heartbeat。
  std::string msg_type;

  // 由客户端生成的请求序号，服务端应在响应中原样返回。
  SeqId seq = 0;

  // 客户端携带的认证令牌，未登录时允许为空。
  std::string token;

  // 存放具体业务字段的 JSON 对象。
  nlohmann::json data;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_COMMON_MESSAGE_H_
