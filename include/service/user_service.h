#ifndef LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_
#define LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_

#include <string>

#include "cache/redis_rate_limiter.h"
#include "common/error_code.h"
#include "common/types.h"
#include "db/user_repository.h"
#include "protocol/auth_messages.h"
#include "server/redis_session_store.h"
#include "server/session_manager.h"

// 本文件声明用户服务层接口。
// Service 层负责编排注册、登录和登出流程，并协调 Repository 与会话状态。
// TODO(lzq): 将密码哈希逻辑迁移到独立 PasswordHasher 组件。
// TODO(lzq): 为登录限流、重复登录策略和审计日志预留扩展点。

namespace chat {

// 表示注册流程的执行结果。
struct RegisterResult {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;

    // 注册成功时返回的业务数据。
    RegisterResponseData data;
    std::uint32_t retry_after_seconds = 0;
};

// 表示登录流程的执行结果。
struct LoginResult {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;

    // 登录成功时返回的业务数据。
    LoginResponseData data;

    // 登录成功时生成的会话状态，需交由网络层（I/O线程）完成绑定。
    ConnectionSession session;
    std::uint32_t retry_after_seconds = 0;
};

// 表示登出流程的执行结果。
struct LogoutResult {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;
};

// 表示查询当前连接登录态的结果。
struct WhoAmIResult {
    // 业务状态码。
    ErrorCode code = ErrorCode::OK;

    // 结果说明文本。
    std::string message;

    // 查询成功时返回当前连接对应的会话信息。
    ConnectionSession data;
};

// 提供用户注册、登录和登出的业务能力。
class UserService {
   public:
    // 使用外部注入的 Repository 和 SessionManager 构造，便于单元测试或替换实现。
    explicit UserService(IUserRepository &user_repository, ISessionManager &session_manager);
    UserService(IUserRepository &user_repository, ISessionManager &session_manager,
                IGlobalSessionStore *global_session_store, IRateLimiter *rate_limiter = nullptr,
                RedisConfig config = {});

    // 使用外部注入的 Repository 构造，便于单元测试或替换实现。
    explicit UserService(IUserRepository &user_repository);

    // 执行用户注册流程。
    RegisterResult registerUser(const RegisterRequest &req, const std::string &identity = "");

    // 执行用户登录流程，并关联当前连接 ID。
    LoginResult login(const LoginRequest &req, ConnectionId conn_id, const std::string &identity = "");

    // 执行用户登出流程，并解除当前连接上的登录态。
    LogoutResult logout(ConnectionId conn_id);

    // 查询当前连接绑定的登录态信息。
    WhoAmIResult whoami(ConnectionId conn_id);

    // 供I/O线程调用的Session绑定方法
    void bindSession(ConnectionId conn_id, const ConnectionSession &session);

    // 显式登出同时撤销 token。
    void logoutSession(ConnectionId conn_id);

    // 连接断开只清理在线状态，token 继续存活到 TTL 到期。
    void clearSession(ConnectionId conn_id);

    // 有效请求用于续期在线状态；失败时保留本地会话。
    void refreshPresence(ConnectionId conn_id);

    // 检查指定连接当前是否仍属于目标用户。
    bool isConnectionBoundToUser(ConnectionId conn_id, UserId user_id) const;

   private:
    // Service 通过抽象接口访问仓储，避免业务逻辑绑定具体实现。
    IUserRepository *user_repository_ = nullptr;

    // 当调用方未注入会话管理器时，服务内部持有一个默认实现。
    SessionManager default_session_manager_;

    // Service 通过抽象接口访问会话管理器，避免业务逻辑绑定具体实现。
    ISessionManager *session_manager_ = &default_session_manager_;

    IGlobalSessionStore *global_session_store_ = nullptr;
    IRateLimiter *rate_limiter_ = nullptr;
    RedisConfig redis_config_;

    // 校验注册请求的字段完整性与基本合法性。
    bool validateRegisterRequest(const RegisterRequest &req, std::string &err) const;

    // 校验登录请求的字段完整性与基本合法性。
    bool validateLoginRequest(const LoginRequest &req, std::string &err) const;

    // 对用户输入密码进行哈希计算。
    std::string hashPassword(const std::string &password) const;

    // 为指定用户生成认证令牌。
    std::string generateToken() const;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_SERVICE_USER_SERVICE_H_
