#include <db/db_connection.h>

using namespace chat;

DbConnection::DbConnection(const DbConfig& config) : config_(config) {}

DbConnection::~DbConnection() noexcept { close(); }

DbConnection::DbConnection(DbConnection&& other) noexcept : conn_(other.conn_), config_(std::move(other.config_)) {
    other.conn_ = nullptr;
}

DbConnection& DbConnection::operator=(DbConnection&& other) noexcept {
    if (this != &other) {
        close();
        conn_ = other.conn_;
        config_ = std::move(other.config_);
        other.conn_ = nullptr;
    }
    return *this;
}

DbConnectionResult DbConnection::connect() {
    if (this->isConnected())
        return DbConnectionResult{true};
    MYSQL* raw = mysql_init(nullptr);
    if (raw == nullptr) {
        return DbConnectionResult{false, 0, "mysql_init failed"};
    }

    auto set_option = [raw](mysql_option option, const void* arg, DbConnectionResult& out_err) -> bool {
        if (mysql_options(raw, option, arg) != 0) {
            out_err.error_code = mysql_errno(raw);
            out_err.error_message = mysql_error(raw);
            mysql_close(raw);
            return false;
        }
        return true;
    };

    DbConnectionResult err_res;
    if (!set_option(MYSQL_OPT_CONNECT_TIMEOUT, &config_.connect_timeout_seconds, err_res))
        return err_res;
    if (!set_option(MYSQL_OPT_READ_TIMEOUT, &config_.read_timeout_seconds, err_res))
        return err_res;
    if (!set_option(MYSQL_OPT_WRITE_TIMEOUT, &config_.write_timeout_seconds, err_res))
        return err_res;
    if (!set_option(MYSQL_SET_CHARSET_NAME, config_.charset.c_str(), err_res))
        return err_res;

    MYSQL* connected = mysql_real_connect(raw, config_.host.c_str(), config_.username.c_str(), config_.password.c_str(),
                                          config_.database.c_str(), config_.port, nullptr, 0);
    if (connected == nullptr) {
        unsigned int err_no = mysql_errno(raw);
        std::string err = mysql_error(raw);
        mysql_close(raw);
        return DbConnectionResult{false, err_no, err};
    }
    conn_ = connected;
    return DbConnectionResult{true};
}

void DbConnection::close() noexcept {
    if (this->isConnected()) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

bool DbConnection::ping() noexcept {
    if (this->isConnected()) {
        return mysql_ping(conn_) == 0;
    }
    return false;
}

bool DbConnection::isConnected() const noexcept { return conn_ != nullptr; }

MYSQL* DbConnection::nativeHandle() noexcept { return conn_; }

const MYSQL* DbConnection::nativeHandle() const noexcept { return conn_; }