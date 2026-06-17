#ifndef LINUX_SERVER_INCLUDE_COMMON_ERROR_CODE_H_
#define LINUX_SERVER_INCLUDE_COMMON_ERROR_CODE_H_

// 本文件集中定义服务器对外暴露的业务错误码。
// 错误码按功能区间划分，便于客户端处理和服务端排障。
//
// TODO(lzq): 为每个错误码补充统一的人类可读错误信息映射。
// TODO(lzq): 后续按认证、聊天、存储等模块拆分错误码段。

namespace chat {

// 表示协议处理与业务执行过程中可能出现的结果状态。
enum class ErrorCode {
    // 请求成功。
    OK = 0,

    // 通用协议与参数错误。
    INVALID_PARAM = 1,
    INVALID_JSON = 2,
    INVALID_PACKET = 3,
    UNKNOWN_MESSAGE_TYPE = 4,

    // 用户与认证相关错误。
    USER_ALREADY_EXISTS = 1001,
    USER_NOT_FOUND = 1002,
    WRONG_PASSWORD = 1003,
    INVALID_CREDENTIALS = 1004,
    USER_ALREADY_ONLINE = 1005,

    // 数据库相关错误。
    DB_INIT_FAILED = 2001,
    DB_QUERY_FAILED = 2002,
    DB_INSERT_FAILED = 2003,

    // 消息与聊天相关错误。
    MESSAGE_TOO_LONG = 3001,
    USER_NOT_ONLINE = 3002,
    CANNOT_SEND_TO_SELF = 3003,
    NOT_LOGGED_IN = 3004,
    IDEMPOTENCY_CONFLICT = 3005,
    RATE_LIMITED = 3006,

    // 好友关系相关错误。
    FRIEND_REQUEST_ALREADY_EXISTS = 4001,
    FRIENDSHIP_ALREADY_EXISTS = 4002,
    FRIEND_REQUEST_NOT_FOUND = 4003,
    FRIENDSHIP_NOT_FOUND = 4004,
    FRIENDSHIP_BLOCKED = 4005,

    // 群组相关错误。
    GROUP_NOT_FOUND = 5001,
    GROUP_MEMBER_ALREADY_EXISTS = 5002,
    PERMISSION_DENIED = 5003,

    // 兜底内部错误。
    INTERNAL_ERROR = 9001,
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_COMMON_ERROR_CODE_H_
