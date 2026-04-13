#ifndef LINUX_SERVER_INCLUDE_MODEL_USER_RECORD_H_
#define LINUX_SERVER_INCLUDE_MODEL_USER_RECORD_H_

#include <string>

#include "common/types.h"

// 本文件定义用户表在服务端内部使用的数据模型。
// 它通常由 Repository 构造并返回给 Service 使用。
//
// TODO(lzq): 根据建表语句确认 status 字段的枚举含义。
// TODO(lzq): 将 created_at 和 updated_at 改为统一时间类型。
// TODO(lzq): 如后续支持手机号和邮箱登录，可补充相应字段。

namespace chat {

// 表示一条用户记录的内存视图。
struct UserRecord {
  // 用户主键 ID。
  UserId id = 0;

  // 用户唯一登录名。
  std::string username;

  // 用户密码哈希值，不保存明文密码。
  std::string password_hash;

  // 用户昵称。
  std::string nickname;

  // 用户状态，例如正常、禁用、注销。
  int status = 0;

  // 用户创建时间。
  std::string created_at;

  // 用户最后更新时间。
  std::string updated_at;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_MODEL_USER_RECORD_H_
