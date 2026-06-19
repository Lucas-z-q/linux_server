#include "db/mysql_statement.h"

#include <cstring>
#include <utility>

namespace chat {

StatementParam StatementParam::String(std::string value) {
    StatementParam param;
    param.type = Type::kString;
    param.string_value = std::move(value);
    return param;
}

StatementParam StatementParam::Int32(std::int32_t value) {
    StatementParam param;
    param.type = Type::kInt32;
    param.int32_value = value;
    return param;
}

StatementParam StatementParam::Int64(std::int64_t value) {
    StatementParam param;
    param.type = Type::kInt64;
    param.int64_value = value;
    return param;
}

MysqlStatement::MysqlStatement(MYSQL* connection) : statement_(mysql_stmt_init(connection)) {}

MysqlStatement::~MysqlStatement() {
    if (statement_ != nullptr) {
        mysql_stmt_close(statement_);
    }
}

bool MysqlStatement::Prepare(const std::string& sql) {
    return statement_ != nullptr && mysql_stmt_prepare(statement_, sql.data(), sql.size()) == 0;
}

bool MysqlStatement::Execute(std::vector<StatementParam>* params) {
    if (statement_ == nullptr || params == nullptr || mysql_stmt_param_count(statement_) != params->size()) {
        return false;
    }

    std::vector<MYSQL_BIND> bindings(params->size());
    std::vector<unsigned long> lengths(params->size());
    for (std::size_t i = 0; i < params->size(); ++i) {
        MYSQL_BIND& binding = bindings[i];
        std::memset(&binding, 0, sizeof(binding));
        StatementParam& param = (*params)[i];
        switch (param.type) {
            case StatementParam::Type::kString:
                lengths[i] = static_cast<unsigned long>(param.string_value.size());
                binding.buffer_type = MYSQL_TYPE_STRING;
                binding.buffer = param.string_value.data();
                binding.buffer_length = lengths[i];
                binding.length = &lengths[i];
                break;
            case StatementParam::Type::kInt32:
                binding.buffer_type = MYSQL_TYPE_LONG;
                binding.buffer = &param.int32_value;
                break;
            case StatementParam::Type::kInt64:
                binding.buffer_type = MYSQL_TYPE_LONGLONG;
                binding.buffer = &param.int64_value;
                break;
        }
    }

    if (!bindings.empty() && mysql_stmt_bind_param(statement_, bindings.data()) != 0) {
        return false;
    }
    return mysql_stmt_execute(statement_) == 0;
}

bool MysqlStatement::StoreResult() { return statement_ != nullptr && mysql_stmt_store_result(statement_) == 0; }

bool MysqlStatement::BindResult(MYSQL_BIND* bindings) {
    return statement_ != nullptr && bindings != nullptr && mysql_stmt_bind_result(statement_, bindings) == 0;
}

int MysqlStatement::Fetch() { return statement_ == nullptr ? 1 : mysql_stmt_fetch(statement_); }

unsigned int MysqlStatement::ErrorNumber() const { return statement_ == nullptr ? 0 : mysql_stmt_errno(statement_); }

my_ulonglong MysqlStatement::AffectedRows() const {
    return statement_ == nullptr ? 0 : mysql_stmt_affected_rows(statement_);
}

my_ulonglong MysqlStatement::InsertId() const { return statement_ == nullptr ? 0 : mysql_stmt_insert_id(statement_); }

MYSQL_STMT* MysqlStatement::NativeHandle() const { return statement_; }

}  // namespace chat
