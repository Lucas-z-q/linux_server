#include "service/user_service.h"

#include <functional>

namespace chat {

RegisterResult UserService::registerUser(const RegisterRequest& req) {
  RegisterResult result;
  std::string err;
  if (!validateRegisterRequest(req, err)) {
    result.code = ErrorCode::INVALID_PARAM;
    result.message = err;
    return result;
  }

  result.code = ErrorCode::INTERNAL_ERROR;
  result.message = "register not implemented";
  return result;
}

LoginResult UserService::login(const LoginRequest& req, ConnectionId conn_id) {
  (void)conn_id;

  LoginResult result;
  std::string err;
  if (!validateLoginRequest(req, err)) {
    result.code = ErrorCode::INVALID_PARAM;
    result.message = err;
    return result;
  }

  result.code = ErrorCode::INTERNAL_ERROR;
  result.message = "login not implemented";
  return result;
}

LogoutResult UserService::logout(ConnectionId conn_id) {
  (void)conn_id;

  LogoutResult result;
  result.code = ErrorCode::INTERNAL_ERROR;
  result.message = "logout not implemented";
  return result;
}

bool UserService::validateRegisterRequest(const RegisterRequest& req,
                                          std::string& err) const {
  if (req.username.empty()) {
    err = "username is empty";
    return false;
  }
  if (req.password.empty()) {
    err = "password is empty";
    return false;
  }
  return true;
}

bool UserService::validateLoginRequest(const LoginRequest& req,
                                       std::string& err) const {
  if (req.username.empty()) {
    err = "username is empty";
    return false;
  }
  if (req.password.empty()) {
    err = "password is empty";
    return false;
  }
  return true;
}

std::string UserService::hashPassword(const std::string& password) const {
  return std::to_string(std::hash<std::string>{}(password));
}

std::string UserService::generateToken(UserId user_id) const {
  return "token_" + std::to_string(user_id);
}

}  // namespace chat
