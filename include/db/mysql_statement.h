#ifndef LINUX_SERVER_INCLUDE_DB_MYSQL_STATEMENT_H_
#define LINUX_SERVER_INCLUDE_DB_MYSQL_STATEMENT_H_

#include <mysql/mysql.h>

#include <cstdint>
#include <string>
#include <vector>

namespace chat {

struct StatementParam {
    enum class Type {
        kString,
        kInt32,
        kInt64,
    };

    static StatementParam String(std::string value);
    static StatementParam Int32(std::int32_t value);
    static StatementParam Int64(std::int64_t value);

    Type type = Type::kString;
    std::string string_value;
    std::int32_t int32_value = 0;
    std::int64_t int64_value = 0;
};

class MysqlStatement {
   public:
    explicit MysqlStatement(MYSQL* connection);
    ~MysqlStatement();

    MysqlStatement(const MysqlStatement&) = delete;
    MysqlStatement& operator=(const MysqlStatement&) = delete;

    bool Prepare(const std::string& sql);
    bool Execute(std::vector<StatementParam>* params);
    bool StoreResult();
    bool BindResult(MYSQL_BIND* bindings);
    int Fetch();

    unsigned int ErrorNumber() const;
    my_ulonglong AffectedRows() const;
    my_ulonglong InsertId() const;
    MYSQL_STMT* NativeHandle() const;

   private:
    MYSQL_STMT* statement_ = nullptr;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_DB_MYSQL_STATEMENT_H_
