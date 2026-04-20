#ifndef LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_

#include <string>

#include "common/error_code.h"
#include "common/types.h"
#include "protocol/auth_messages.h"
#include "db/user_repository.h"

// 本文件声明用户服务层接口。
// Service 层负责编排注册、登录和登出流程，并协调 Repository 与会话状态。
//
// TODO(lzq): 通过依赖注入引入 UserRepository 和 SessionManager。
// TODO(lzq): 将密码哈希逻辑迁移到独立 PasswordHasher 组件。
// TODO(lzq): 为登录限流、重复登录策略和审计日志预留扩展点。

namespace chat
{

  // 表示注册流程的执行结果。
  struct RegisterResult
  {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;

    // 注册成功时返回的业务数据。
    RegisterResponseData data;
  };

  // 表示登录流程的执行结果。
  struct LoginResult
  {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;

    // 登录成功时返回的业务数据。
    LoginResponseData data;
  };

  // 表示登出流程的执行结果。
  struct LogoutResult
  {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;
  };

  // 提供用户注册、登录和登出的业务能力。
  class UserService
  {
  public:
    UserService() {};

    // 执行用户注册流程。
    RegisterResult registerUser(const RegisterRequest &req);

    // 执行用户登录流程，并关联当前连接 ID。
    LoginResult login(const LoginRequest &req, ConnectionId conn_id);

    // 执行用户登出流程，并解除当前连接上的登录态。
    LogoutResult logout(ConnectionId conn_id);

  private:
    UserRepository ur;

    // 校验注册请求的字段完整性与基本合法性。
    bool validateRegisterRequest(const RegisterRequest &req,
                                 std::string &err) const;

    // 校验登录请求的字段完整性与基本合法性。
    bool validateLoginRequest(const LoginRequest &req,
                              std::string &err) const;

    // 对用户输入密码进行哈希计算。
    std::string hashPassword(const std::string &password) const;

    // 为指定用户生成认证令牌。
    std::string generateToken(UserId user_id) const;
  };

} // namespace chat

#endif // LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_
