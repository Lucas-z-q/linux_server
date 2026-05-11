#include "service/user_service.h"

namespace chat {

UserService::UserService() = default;

UserService::UserService(IUserRepository &user_repository) : user_repository_(&user_repository) {}

UserService::UserService(IUserRepository &user_repository, ISessionManager &session_manager)
    : user_repository_(&user_repository), session_manager_(&session_manager) {}

UserService::UserService(ISessionManager &session_manager) : session_manager_(&session_manager) {}

RegisterResult UserService::registerUser(const RegisterRequest &req) {
    RegisterResult result;
    std::string err;
    if (!validateRegisterRequest(req, err)) {
        result.code = ErrorCode::INVALID_PARAM;
        result.message = err;
        return result;
    }

    // 注册流程的第一步是查重；这里必须区分“没找到用户”和“查询失败”，
    // 否则 Service 会把数据库故障误判成“可以继续注册”。
    const FindUserResult find_result = user_repository_->findByUsername(req.username);
    if (find_result.status == RepositoryStatus::kOk) {
        result.code = ErrorCode::USER_ALREADY_EXISTS;
        result.message = "username already exists";
        return result;
    }
    if (find_result.status == RepositoryStatus::kQueryFailed) {
        result.code = ErrorCode::DB_QUERY_FAILED;
        result.message = "query user failed";
        return result;
    }
    if (find_result.status != RepositoryStatus::kNotFound) {
        result.code = ErrorCode::INTERNAL_ERROR;
        result.message = "unexpected repository status";
        return result;
    }

    const std::string password_hash = hashPassword(req.password);

    // 插入阶段仍要处理重复键，避免并发注册时把唯一键冲突误报成普通插入失败。
    const CreateUserResult create_result = user_repository_->createUser(req.username, password_hash, req.nickname);
    if (create_result.status == RepositoryStatus::kDuplicate) {
        result.code = ErrorCode::USER_ALREADY_EXISTS;
        result.message = "username already exists";
        return result;
    }
    if (create_result.status == RepositoryStatus::kInsertFailed) {
        result.code = ErrorCode::DB_INSERT_FAILED;
        result.message = "create user failed";
        return result;
    }
    if (create_result.status != RepositoryStatus::kOk) {
        result.code = ErrorCode::INTERNAL_ERROR;
        result.message = "unexpected repository status";
        return result;
    }

    result.data.user_id = create_result.user_id;
    result.code = ErrorCode::OK;
    result.message = "register success";

    return result;
}

LoginResult UserService::login(const LoginRequest &req, ConnectionId conn_id) {
    LoginResult result;
    std::string err;
    if (!validateLoginRequest(req, err)) {
        result.code = ErrorCode::INVALID_PARAM;
        result.message = err;
        return result;
    }

    const FindUserResult find_result = user_repository_->findByUsername(req.username);
    if (find_result.status == RepositoryStatus::kQueryFailed) {
        result.code = ErrorCode::DB_QUERY_FAILED;
        result.message = "query user failed";
        return result;
    }
    if (find_result.status == RepositoryStatus::kNotFound) {
        result.code = ErrorCode::INVALID_CREDENTIALS;
        result.message = "invalid username or password";
        return result;
    }
    if (find_result.status != RepositoryStatus::kOk || !find_result.user) {
        result.code = ErrorCode::INTERNAL_ERROR;
        result.message = "unexpected repository result";
        return result;
    }
    const UserRecord &user = find_result.user.value();

    const std::string password_hash = hashPassword(req.password);
    if (password_hash != user.password_hash) {
        result.code = ErrorCode::INVALID_CREDENTIALS;
        result.message = "invalid username or password";
        return result;
    }

    const std::string token = generateToken(user.id);
    ConnectionSession session;
    session.authenticated = true;
    session.user_id = user.id;
    session.username = user.username;
    session.token = token;

    result.session = session;
    result.data.user_id = user.id;
    result.data.nickname = user.nickname;
    result.data.token = token;
    result.code = ErrorCode::OK;
    result.message = "login success";
    return result;
}

LogoutResult UserService::logout(ConnectionId conn_id) {
    LogoutResult result;

    if (!session_manager_->GetSession(conn_id).has_value()) {
        result.code = ErrorCode::USER_NOT_FOUND;
        result.message = "user not logged in";
        return result;
    }

    result.code = ErrorCode::OK;
    result.message = "logout success";
    return result;
}

WhoAmIResult UserService::whoami(ConnectionId conn_id) {
    WhoAmIResult result;
    const std::optional<ConnectionSession> session = session_manager_->GetSession(conn_id);
    if (!session.has_value() || !session->authenticated) {
        result.code = ErrorCode::USER_NOT_FOUND;
        result.message = "user not logged in";
        return result;
    }

    result.code = ErrorCode::OK;
    result.message = "ok";
    result.data = *session;
    return result;
}

void UserService::bindSession(ConnectionId conn_id, const ConnectionSession &session) {
    session_manager_->BindSession(conn_id, session);
}

void UserService::clearSession(ConnectionId conn_id) { session_manager_->ClearSession(conn_id); }

bool UserService::validateRegisterRequest(const RegisterRequest &req, std::string &err) const {
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

bool UserService::validateLoginRequest(const LoginRequest &req, std::string &err) const {
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

std::string UserService::hashPassword(const std::string &password) const {
    return std::to_string(std::hash<std::string>{}(password));
}

std::string UserService::generateToken(UserId user_id) const { return "token_" + std::to_string(user_id); }

}  // namespace chat
