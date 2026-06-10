#include "service/user_service.h"

#include <sys/random.h>

#include <array>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>

namespace chat {

UserService::UserService(IUserRepository &user_repository) : user_repository_(&user_repository) {}

UserService::UserService(IUserRepository &user_repository, ISessionManager &session_manager)
    : user_repository_(&user_repository), session_manager_(&session_manager) {}

UserService::UserService(IUserRepository &user_repository, ISessionManager &session_manager,
                         IGlobalSessionStore *global_session_store, IRateLimiter *rate_limiter, RedisConfig config)
    : user_repository_(&user_repository),
      session_manager_(&session_manager),
      global_session_store_(global_session_store),
      rate_limiter_(rate_limiter),
      redis_config_(std::move(config)) {}

RegisterResult UserService::registerUser(const RegisterRequest &req, const std::string &identity) {
    RegisterResult result;
    std::string err;
    if (!validateRegisterRequest(req, err)) {
        result.code = ErrorCode::INVALID_PARAM;
        result.message = err;
        return result;
    }
    if (rate_limiter_ != nullptr && !identity.empty()) {
        const RateLimitResult limit = rate_limiter_->Allow("register", identity, redis_config_.register_rate_limit,
                                                           redis_config_.register_rate_window_seconds);
        if (!limit.allowed) {
            result.code = ErrorCode::RATE_LIMITED;
            result.message = "register rate limit exceeded";
            result.retry_after_seconds = limit.retry_after_seconds;
            return result;
        }
    }

    // 注册流程的第一步是查重；这里必须区分“没找到用户”和“查询失败”，
    // 否则 Service 会把数据库故障误判成“可以继续注册”。
    const FindUserResult find_result = user_repository_->findByUsername(req.username);
    if (find_result.status == RepositoryStatus::kOk) {
        result.code = ErrorCode::USER_ALREADY_EXISTS;
        result.message = "username already exists";
        return result;
    }
    if (find_result.status == RepositoryStatus::kQueryFailed ||
        find_result.status == RepositoryStatus::kConnectionUnavailable ||
        find_result.status == RepositoryStatus::kBorrowTimeout) {
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
    if (create_result.status == RepositoryStatus::kInsertFailed ||
        create_result.status == RepositoryStatus::kConnectionUnavailable ||
        create_result.status == RepositoryStatus::kBorrowTimeout) {
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

LoginResult UserService::login(const LoginRequest &req, ConnectionId conn_id, const std::string &identity) {
    LoginResult result;
    std::string err;
    if (!validateLoginRequest(req, err)) {
        result.code = ErrorCode::INVALID_PARAM;
        result.message = err;
        return result;
    }
    if (rate_limiter_ != nullptr && !identity.empty()) {
        const RateLimitResult limit = rate_limiter_->Allow("login", identity, redis_config_.login_rate_limit,
                                                           redis_config_.login_rate_window_seconds);
        if (!limit.allowed) {
            result.code = ErrorCode::RATE_LIMITED;
            result.message = "login rate limit exceeded";
            result.retry_after_seconds = limit.retry_after_seconds;
            return result;
        }
    }

    const FindUserResult find_result = user_repository_->findByUsername(req.username);
    if (find_result.status == RepositoryStatus::kQueryFailed ||
        find_result.status == RepositoryStatus::kConnectionUnavailable ||
        find_result.status == RepositoryStatus::kBorrowTimeout) {
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

    const std::string token = generateToken();
    if (token.empty()) {
        result.code = ErrorCode::INTERNAL_ERROR;
        result.message = "generate token failed";
        return result;
    }
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
    if (!session_manager_->BindSession(conn_id, session) || global_session_store_ == nullptr) {
        return;
    }
    const Timestamp issued_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    // Redis 是全局加速层。运行期失败不能撤销已经建立的本地连接态。
    global_session_store_->Bind(conn_id, session, issued_at);
}

void UserService::logoutSession(ConnectionId conn_id) {
    const std::optional<ConnectionSession> session = session_manager_->GetSession(conn_id);
    if (!session) {
        return;
    }
    session_manager_->ClearSession(conn_id);
    if (global_session_store_ != nullptr) {
        global_session_store_->ClearPresence(conn_id, *session);
        global_session_store_->RevokeToken(session->token);
    }
}

void UserService::clearSession(ConnectionId conn_id) {
    const std::optional<ConnectionSession> session = session_manager_->GetSession(conn_id);
    if (!session) {
        return;
    }
    session_manager_->ClearSession(conn_id);
    if (global_session_store_ != nullptr) {
        global_session_store_->ClearPresence(conn_id, *session);
    }
}

void UserService::refreshPresence(ConnectionId conn_id) {
    const std::optional<ConnectionSession> session = session_manager_->GetSession(conn_id);
    if (session && global_session_store_ != nullptr) {
        global_session_store_->Refresh(conn_id, *session);
    }
}

bool UserService::isConnectionBoundToUser(ConnectionId conn_id, UserId user_id) const {
    auto session_opt = session_manager_->GetSession(conn_id);
    return session_opt.has_value() && session_opt->authenticated && session_opt->user_id == user_id;
}

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

std::string UserService::generateToken() const {
    std::array<unsigned char, 32> bytes{};
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count <= 0) {
            return "";
        }
        offset += static_cast<std::size_t>(count);
    }

    std::ostringstream token;
    token << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        token << std::setw(2) << static_cast<int>(byte);
    }
    return token.str();
}

}  // namespace chat
