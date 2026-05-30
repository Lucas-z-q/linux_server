#ifndef LINUX_SERVER_INCLUDE_PROTOCOL_PROTOCOL_HELPER_H_
#define LINUX_SERVER_INCLUDE_PROTOCOL_PROTOCOL_HELPER_H_

#include <string>

#include "common/error_code.h"
#include "common/message.h"
#include "common/response.h"

// 本文件声明协议层辅助函数。
// 这些函数用于减少 handler 中重复的响应构造和消息类型判断代码。
//
// TODO(lzq): 为更多消息类型补充统一的请求名与响应名映射。
// TODO(lzq): 增加通用字段校验工具，例如 seq 和 token 合法性检查。
// TODO(lzq): 把协议辅助函数纳入单元测试，保证错误响应格式稳定。

namespace chat::protocol {

// 根据请求类型生成对应的响应类型名称。
std::string makeResponseType(const std::string& request_type);

// 基于请求信封构造一个默认成功响应。
Response makeSuccessResponse(const Message& request);

// 基于请求信封构造一个统一错误响应。
Response makeErrorResponse(const Message& request, ErrorCode code, const std::string& message);

// 判断给定消息类型是否属于认证相关消息。
bool isAuthMessage(const std::string& msg_type);

}  // namespace chat::protocol

#endif  // LINUX_SERVER_INCLUDE_PROTOCOL_PROTOCOL_HELPER_H_
