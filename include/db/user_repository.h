#ifndef LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_

#include <optional>
#include <string>

#include "common/types.h"
#include "model/user_record.h"

// 本文件声明用户数据访问层接口。
// Repository 只负责用户表读写，不负责密码校验和登录态维护。
//
// TODO(lzq): 通过构造函数注入 DbPool，替换当前占位设计。
// TODO(lzq): 为查询失败与空结果建立更清晰的错误返回模型。
// TODO(lzq): 补充按照用户名唯一索引查询的 SQL 约束说明。

namespace chat {

// 封装用户表相关的数据库访问操作。
class UserRepository {
 public:
  UserRepository() = default;

  // 按用户名查询用户记录；若不存在则返回空。
  std::optional<UserRecord> findByUsername(const std::string& username);

  // 按用户 ID 查询用户记录；若不存在则返回空。
  std::optional<UserRecord> findById(UserId user_id);

  // 创建新用户，并在成功时回填生成的用户 ID。
  bool createUser(const std::string& username, const std::string& password_hash,
                  const std::string& nickname, UserId& user_id);
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_
