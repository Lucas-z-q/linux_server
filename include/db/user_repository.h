#ifndef LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_
#define LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_

#include <optional>
#include <string>

#include "common/types.h"
#include "model/user_record.h"

// 本文件声明用户数据访问层接口。
// Repository 只负责用户表读写，不负责密码校验和登录态维护。

namespace chat {

// 表示 Repository 层对一次数据库操作的结果分类。
//
// 该枚举不直接暴露给客户端；它只用于在 Repository 与 Service 之间
// 传递足够精确的内部状态，避免 Service 只能通过 optional/bool 猜测原因。
enum class RepositoryStatus {
    kOk = 0,
    kNotFound,
    kQueryFailed,
    kDuplicate,
    kInsertFailed,
    kConnectionUnavailable,
    kBorrowTimeout,
};

// 封装一次“查询用户”操作的结果。
struct FindUserResult {
    // 查询状态。
    RepositoryStatus status = RepositoryStatus::kNotFound;

    // 查询成功且命中记录时返回的用户数据。
    std::optional<UserRecord> user;
};

// 封装一次“创建用户”操作的结果。
struct CreateUserResult {
    // 插入状态。
    RepositoryStatus status = RepositoryStatus::kInsertFailed;

    // 插入成功后返回的新用户 ID。
    UserId user_id = 0;
};

// 定义用户数据访问的抽象接口，便于 Service 层注入测试替身。
class IUserRepository {
   public:
    virtual ~IUserRepository() = default;

    // 按用户名查询用户记录。
    // status 为 kOk 时，user 中包含命中的用户数据。
    virtual FindUserResult findByUsername(const std::string& username) = 0;

    // 按用户 ID 查询用户记录。
    // status 为 kNotFound 时，表示查询成功但未命中记录。
    virtual FindUserResult findById(UserId user_id) = 0;

    // 创建新用户。
    // status 为 kOk 时，user_id 中包含新生成的主键。
    virtual CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                        const std::string& nickname) = 0;

    virtual RepositoryStatus updatePasswordHash(UserId, const std::string&) { return RepositoryStatus::kQueryFailed; }
};

class DbPool;
// 封装用户表相关的数据库访问操作。
class UserRepository : public IUserRepository {
   public:
    explicit UserRepository(DbPool* pool);

    // 按用户名查询用户记录，并返回结构化查询状态。
    FindUserResult findByUsername(const std::string& username) override;

    // 按用户 ID 查询用户记录，并返回结构化查询状态。
    FindUserResult findById(UserId user_id) override;

    // 创建新用户，并返回插入状态与新生成的用户 ID。
    CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                const std::string& nickname) override;

    RepositoryStatus updatePasswordHash(UserId user_id, const std::string& password_hash) override;

   private:
    DbPool* pool_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_USER_REPOSITORY_H_
