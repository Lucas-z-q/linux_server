#include "service/user_service.h"

#include <functional>
#include <optional>

namespace chat
{

  RegisterResult UserService::registerUser(const RegisterRequest &req)
  {
    RegisterResult result;
    std::string err;
    if (!validateRegisterRequest(req, err))
    {
      result.code = ErrorCode::INVALID_PARAM;
      result.message = err;
      return result;
    }

    // use UserRepository::findByUsername
    std::optional<UserRecord> query_res = ur.findByUsername(req.username);
    if (query_res != std::nullopt)
    {
      result.code = ErrorCode::USER_ALREADY_EXISTS;
      result.message = "username already exists";
      return result;
    }

    std::string password_hash = hashPassword(req.password);

    if (!ur.createUser(req.username, password_hash, req.nickname, result.data.user_id))
    {
      result.code = ErrorCode::DB_INSERT_FAILED;
      result.message = "create user failed.";
      return result;
    }

    result.code = ErrorCode::OK;
    result.message = "register success.";

    return result;
  }

  LoginResult UserService::login(const LoginRequest &req, ConnectionId conn_id)
  {
    (void)conn_id;

    LoginResult result;
    std::string err;
    if (!validateLoginRequest(req, err))
    {
      result.code = ErrorCode::INVALID_PARAM;
      result.message = err;
      return result;
    }

    result.code = ErrorCode::INTERNAL_ERROR;
    result.message = "login not implemented";
    return result;
  }

  LogoutResult UserService::logout(ConnectionId conn_id)
  {
    (void)conn_id;

    LogoutResult result;
    result.code = ErrorCode::INTERNAL_ERROR;
    result.message = "logout not implemented";
    return result;
  }

  bool UserService::validateRegisterRequest(const RegisterRequest &req,
                                            std::string &err) const
  {
    if (req.username.empty())
    {
      err = "username is empty";
      return false;
    }
    if (req.password.empty())
    {
      err = "password is empty";
      return false;
    }
    return true;
  }

  bool UserService::validateLoginRequest(const LoginRequest &req,
                                         std::string &err) const
  {
    if (req.username.empty())
    {
      err = "username is empty";
      return false;
    }
    if (req.password.empty())
    {
      err = "password is empty";
      return false;
    }
    return true;
  }

  std::string UserService::hashPassword(const std::string &password) const
  {
    return std::to_string(std::hash<std::string>{}(password));
  }

  std::string UserService::generateToken(UserId user_id) const
  {
    return "token_" + std::to_string(user_id);
  }

} // namespace chat
