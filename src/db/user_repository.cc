#include "db/user_repository.h"

#include <mysql/mysql.h>

#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/logger.h"
#include "db/db_pool.h"

// 本文件实现 users 表的最小数据访问能力。
// Repository 向业务层屏蔽 SQL 细节，同时通过接入 DbPool 实现物理连接的高效复用。
namespace {

constexpr unsigned int kMysqlDuplicateEntryError = 1062;
constexpr unsigned int kMysqlServerGoneError = 2006;
constexpr unsigned int kMysqlServerLost = 2013;

bool IsConnectionError(unsigned int err_no) { return err_no == kMysqlServerGoneError || err_no == kMysqlServerLost; }

// 为查询结果集提供自动释放逻辑，降低手工管理资源的出错概率。
struct MysqlResultDeleter {
    void operator()(MYSQL_RES *result) const {
        if (result != nullptr) {
            mysql_free_result(result);
        }
    }
};

using MysqlResultPtr = std::unique_ptr<MYSQL_RES, MysqlResultDeleter>;

void LogMysqlError(MYSQL *conn, const std::string &action) {
    const unsigned int err_no = conn != nullptr ? mysql_errno(conn) : 0;
    LOG_ERROR("UserRepository") << "action=" << action << " failed mysql_errno=" << err_no;
}

// 在拼接 SQL 前对字符串做转义，避免引号等字符破坏语句结构。
std::string EscapeSqlValue(MYSQL *conn, const std::string &input) {
    std::vector<char> buffer(input.size() * 2 + 1, '\0');
    const unsigned long escaped_len =
        mysql_real_escape_string(conn, buffer.data(), input.c_str(), static_cast<unsigned long>(input.size()));
    return std::string(buffer.data(), escaped_len);
}

// 执行一条预期只返回单行用户记录的查询，并把结果映射为 FindUserResult。
chat::FindUserResult ReadSingleUser(chat::PooledConnection &conn, const std::string &query) {
    MYSQL *raw_conn = conn->nativeHandle();

    if (mysql_query(raw_conn, query.c_str()) != 0) {
        unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "mysql_query");
        if (IsConnectionError(err_no)) {
            conn.markBad();  // 标记底层物理连接失效，避免池化复用
        }
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MysqlResultPtr result(mysql_store_result(raw_conn));
    if (result == nullptr) {
        unsigned int err_no = mysql_errno(raw_conn);
        LogMysqlError(raw_conn, "mysql_store_result");
        if (IsConnectionError(err_no)) {
            conn.markBad();
        }
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row == nullptr) {
        return {.status = chat::RepositoryStatus::kNotFound};
    }

    const unsigned long *lengths = mysql_fetch_lengths(result.get());
    if (lengths == nullptr) {
        LogMysqlError(raw_conn, "mysql_fetch_lengths");
        return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    chat::UserRecord record;
    record.id = std::strtoll(row[0], nullptr, 10);
    record.username.assign(row[1], lengths[1]);
    record.password_hash.assign(row[2], lengths[2]);
    record.nickname.assign(row[3], lengths[3]);
    record.status = std::atoi(row[4]);
    record.created_at.assign(row[5], lengths[5]);
    record.updated_at.assign(row[6], lengths[6]);
    return {.status = chat::RepositoryStatus::kOk, .user = record};
}

}  // namespace

namespace chat {

UserRepository::UserRepository(DbPool *pool) : pool_(pool) {}

FindUserResult UserRepository::findByUsername(const std::string &username) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        RepositoryStatus status = (borrow_res.error == DbPoolError::kBorrowTimeout)
                                      ? RepositoryStatus::kBorrowTimeout
                                      : RepositoryStatus::kConnectionUnavailable;
        return {.status = status};
    }
    auto &conn = *borrow_res.connection;

    const std::string escaped_username = EscapeSqlValue(conn->nativeHandle(), username);
    const std::string query =
        "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
        "FROM users WHERE username='" +
        escaped_username + "' LIMIT 1";
    return ReadSingleUser(conn, query);
}

FindUserResult UserRepository::findById(UserId user_id) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        RepositoryStatus status = (borrow_res.error == DbPoolError::kBorrowTimeout)
                                      ? RepositoryStatus::kBorrowTimeout
                                      : RepositoryStatus::kConnectionUnavailable;
        return {.status = status};
    }
    auto &conn = *borrow_res.connection;

    const std::string query =
        "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
        "FROM users WHERE id=" +
        std::to_string(user_id) + " LIMIT 1";
    return ReadSingleUser(conn, query);
}

CreateUserResult UserRepository::createUser(const std::string &username, const std::string &password_hash,
                                            const std::string &nickname) {
    auto borrow_res = pool_->borrow();
    if (!borrow_res.ok()) {
        RepositoryStatus status = (borrow_res.error == DbPoolError::kBorrowTimeout)
                                      ? RepositoryStatus::kBorrowTimeout
                                      : RepositoryStatus::kConnectionUnavailable;
        return {.status = status};
    }
    auto &conn = *borrow_res.connection;

    const std::string escaped_username = EscapeSqlValue(conn->nativeHandle(), username);
    const std::string escaped_password_hash = EscapeSqlValue(conn->nativeHandle(), password_hash);
    const std::string escaped_nickname = EscapeSqlValue(conn->nativeHandle(), nickname);
    const std::string query = "INSERT INTO users(username, password_hash, nickname, status) VALUES('" +
                              escaped_username + "','" + escaped_password_hash + "','" + escaped_nickname + "',1)";

    if (mysql_query(conn->nativeHandle(), query.c_str()) != 0) {
        unsigned int err_no = mysql_errno(conn->nativeHandle());
        if (err_no == kMysqlDuplicateEntryError) {
            return {.status = RepositoryStatus::kDuplicate};
        }
        LogMysqlError(conn->nativeHandle(), "insert user");
        if (IsConnectionError(err_no)) {
            conn.markBad();
        }
        return {.status = RepositoryStatus::kInsertFailed};
    }

    const UserId user_id = static_cast<UserId>(mysql_insert_id(conn->nativeHandle()));
    if (user_id <= 0) {
        return {.status = RepositoryStatus::kInsertFailed};
    }
    return {.status = RepositoryStatus::kOk, .user_id = user_id};
}

}  // namespace chat
