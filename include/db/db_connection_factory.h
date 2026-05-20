#ifndef LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_FACTORY_H_
#define LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_FACTORY_H_

#include <memory>
#include "db/db_connection.h"

namespace chat {

/**
 * @brief Interface for DB Connection Factory.
 * 
 * NOTE: Implementations of IDbConnectionFactory must be thread-safe, as
 * DbPool::createConnection() invokes createConnection() concurrently without holding
 * the internal pool lock. This avoids executing slow network connection establishment 
 * while blocking other threads from borrowing/returning connections.
 */
class IDbConnectionFactory {
   public:
    virtual ~IDbConnectionFactory() = default;
    virtual std::unique_ptr<DbConnection> createConnection(const DbConfig& config) = 0;
};

class DefaultDbConnectionFactory : public IDbConnectionFactory {
   public:
    std::unique_ptr<DbConnection> createConnection(const DbConfig& config) override {
        return std::make_unique<DbConnection>(config);
    }
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_DB_CONNECTION_FACTORY_H_
