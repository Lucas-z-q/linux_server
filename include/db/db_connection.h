#ifndef LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_H_
#define LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_H_

#include <mysql/mysql.h>

#include <string>

#include "config/db_config.h"

namespace chat {

struct DbConnectionResult {
    bool success = false;
    unsigned int error_code = 0;
    std::string error_message;

    bool ok() const noexcept { return success; }
};

class DbConnection {
   public:
    explicit DbConnection(const DbConfig& config);
    virtual ~DbConnection() noexcept;

    DbConnection(const DbConnection&) = delete;
    DbConnection& operator=(const DbConnection&) = delete;

    DbConnection(DbConnection&& other) noexcept;
    DbConnection& operator=(DbConnection&& other) noexcept;

    virtual DbConnectionResult connect();
    virtual void close() noexcept;
    virtual bool ping() noexcept;

    virtual bool isConnected() const noexcept;

    // 非拥有指针，调用者不得关闭或长期保存。
    MYSQL* nativeHandle() noexcept;
    const MYSQL* nativeHandle() const noexcept;

   private:
    MYSQL* conn_ = nullptr;
    DbConfig config_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_H_
