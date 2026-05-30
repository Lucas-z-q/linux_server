#ifndef LINUX_SERVER_INCLUDE_COMMON_RESPONSE_H_
#define LINUX_SERVER_INCLUDE_COMMON_RESPONSE_H_

#include <nlohmann/json.hpp>
#include <string>

#include "common/error_code.h"
#include "common/types.h"

// 本文件定义服务端返回给客户端的通用响应信封。
// 响应统一携带状态码、描述信息与业务数据，便于客户端处理。
//
// TODO(lzq): 为常见响应补充标准 message 文案生成函数。
// TODO(lzq): 统一 data 为空时的编码策略，避免客户端解析分支过多。
// TODO(lzq): 评估是否需要增加 trace_id 以支持链路追踪。

namespace chat {

// 表示一条从服务端返回给客户端的通用协议响应。
struct Response {
    // 响应消息类型，通常为请求类型后追加 _resp。
    std::string msg_type;

    // 与请求对应的序号，用于客户端匹配异步响应。
    SeqId seq = 0;

    // 响应结果状态码。
    ErrorCode code = ErrorCode::OK;

    // 便于日志与客户端展示的简短文本说明。
    std::string message;

    // 存放具体业务返回数据的 JSON 对象。
    nlohmann::json data;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_COMMON_RESPONSE_H_
