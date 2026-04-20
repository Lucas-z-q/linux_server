#include "db/user_repository.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 本文件实现 users 表的最小数据访问能力。
// 当前设计刻意把“建连、转义、查询、结果映射”都收口在 Repository 内部，
// 这样 Service 层只需要关心业务流程，不需要直接接触 SQL 细节。

namespace
{

  // 这里手动声明 MySQL C API 所需的不透明类型和函数。
  // 当前环境只有运行时库，没有标准开发头文件，因此采用最小声明集完成编译。
  struct st_mysql;
  struct st_mysql_res;
  using MYSQL = st_mysql;
  using MYSQL_RES = st_mysql_res;
  using MYSQL_ROW = char **;

  extern "C"
  {
    MYSQL *mysql_init(MYSQL *mysql);
    MYSQL *mysql_real_connect(MYSQL *mysql, const char *host, const char *user,
                              const char *passwd, const char *db,
                              unsigned int port, const char *unix_socket,
                              unsigned long client_flag);
    void mysql_close(MYSQL *sock);
    int mysql_query(MYSQL *mysql, const char *q);
    MYSQL_RES *mysql_store_result(MYSQL *mysql);
    MYSQL_ROW mysql_fetch_row(MYSQL_RES *result);
    unsigned long *mysql_fetch_lengths(MYSQL_RES *result);
    void mysql_free_result(MYSQL_RES *result);
    const char *mysql_error(MYSQL *mysql);
    unsigned int mysql_errno(MYSQL *mysql);
    unsigned long mysql_real_escape_string(MYSQL *mysql, char *to,
                                           const char *from, unsigned long length);
    unsigned long long mysql_insert_id(MYSQL *mysql);
  }

  constexpr unsigned int kMysqlDuplicateEntryError = 1062;

  // 为 unique_ptr 提供 MySQL 连接释放逻辑，避免每个返回路径都手动 close。
  struct MysqlDeleter
  {
    void operator()(MYSQL *conn) const
    {
      if (conn != nullptr)
      {
        mysql_close(conn);
      }
    }
  };

  // 为查询结果集提供自动释放逻辑，降低手工管理资源的出错概率。
  struct MysqlResultDeleter
  {
    void operator()(MYSQL_RES *result) const
    {
      if (result != nullptr)
      {
        mysql_free_result(result);
      }
    }
  };

  using MysqlPtr = std::unique_ptr<MYSQL, MysqlDeleter>;
  using MysqlResultPtr = std::unique_ptr<MYSQL_RES, MysqlResultDeleter>;

  // 读取环境变量；未设置时回退到给定默认值。
  std::string GetEnvOrDefault(const char *name, const std::string &default_value)
  {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
      return default_value;
    }
    return value;
  }

  // 尝试把端口环境变量解析为 uint16_t；非法输入统一回退默认端口。
  std::uint16_t ParsePortOrDefault(const char *value, std::uint16_t default_port)
  {
    if (value == nullptr || *value == '\0')
    {
      return default_port;
    }

    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' ||
        parsed > std::numeric_limits<std::uint16_t>::max())
    {
      return default_port;
    }
    return static_cast<std::uint16_t>(parsed);
  }

  // 默认构造的 Repository 从环境变量装载数据库配置，
  // 方便本地手工测试时无需在代码里硬编码账号信息。
  chat::DbConfig LoadDbConfigFromEnv()
  {
    chat::DbConfig config;
    config.host = GetEnvOrDefault("CHAT_DB_HOST", config.host);
    config.port =
        ParsePortOrDefault(std::getenv("CHAT_DB_PORT"), config.port);
    config.username = GetEnvOrDefault("CHAT_DB_USER", "");
    config.password = GetEnvOrDefault("CHAT_DB_PASSWORD", "");
    config.database = GetEnvOrDefault("CHAT_DB_NAME", "");
    return config;
  }

  // 当前阶段只校验最基本的建连必需项。
  bool IsConfigUsable(const chat::DbConfig &config)
  {
    return !config.host.empty() && !config.username.empty() &&
           !config.database.empty();
  }

  // 统一输出 MySQL 错误，便于后续排查连接、查询、插入失败原因。
  void LogMysqlError(MYSQL *conn, const std::string &action)
  {
    const unsigned int err_no = conn != nullptr ? mysql_errno(conn) : 0;
    const char *err_msg = conn != nullptr ? mysql_error(conn) : "unknown mysql error";
    std::cerr << "[user_repository] " << action << " failed";
    if (err_no != 0)
    {
      std::cerr << " errno=" << err_no;
    }
    std::cerr << " message=" << err_msg << std::endl;
  }

  // 每次仓储调用都按需建立一个短连接。
  // 这样实现最简单，适合当前教学/早期阶段；后续可替换为真正连接池。
  MysqlPtr Connect(const chat::DbConfig &config)
  {
    if (!IsConfigUsable(config))
    {
      std::cerr << "[user_repository] db config is incomplete; "
                << "set CHAT_DB_HOST/CHAT_DB_PORT/CHAT_DB_USER/CHAT_DB_PASSWORD/CHAT_DB_NAME"
                << std::endl;
      return nullptr;
    }

    MysqlPtr conn(mysql_init(nullptr));
    if (conn == nullptr)
    {
      std::cerr << "[user_repository] mysql_init failed" << std::endl;
      return nullptr;
    }

    MYSQL *raw = mysql_real_connect(
        conn.get(), config.host.c_str(), config.username.c_str(),
        config.password.c_str(), config.database.c_str(),
        static_cast<unsigned int>(config.port), nullptr, 0);
    if (raw == nullptr)
    {
      LogMysqlError(conn.get(), "mysql_real_connect");
      return nullptr;
    }
    return conn;
  }

  // 在拼接 SQL 前对字符串做转义，避免引号等字符破坏语句结构。
  std::string EscapeSqlValue(MYSQL *conn, const std::string &input)
  {
    std::vector<char> buffer(input.size() * 2 + 1, '\0');
    const unsigned long escaped_len = mysql_real_escape_string(
        conn, buffer.data(), input.c_str(), static_cast<unsigned long>(input.size()));
    return std::string(buffer.data(), escaped_len);
  }

  // 执行一条预期只返回单行用户记录的查询，并把结果映射为 FindUserResult。
  // 这样 Service 可以区分“确实没查到”和“查询本身失败”。
  chat::FindUserResult ReadSingleUser(MYSQL *conn,
                                      const std::string &query)
  {
    if (mysql_query(conn, query.c_str()) != 0)
    {
      LogMysqlError(conn, "mysql_query");
      return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MysqlResultPtr result(mysql_store_result(conn));
    if (result == nullptr)
    {
      LogMysqlError(conn, "mysql_store_result");
      return {.status = chat::RepositoryStatus::kQueryFailed};
    }

    MYSQL_ROW row = mysql_fetch_row(result.get());
    if (row == nullptr)
    {
      return {.status = chat::RepositoryStatus::kNotFound};
    }

    const unsigned long *lengths = mysql_fetch_lengths(result.get());
    if (lengths == nullptr)
    {
      LogMysqlError(conn, "mysql_fetch_lengths");
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

} // namespace

namespace chat
{

  UserRepository::UserRepository() : config_(LoadDbConfigFromEnv()) {}

  UserRepository::UserRepository(const DbConfig &config) : config_(config) {}

  FindUserResult UserRepository::findByUsername(
      const std::string &username)
  {
    MysqlPtr conn = Connect(config_);
    if (conn == nullptr)
    {
      return {.status = RepositoryStatus::kQueryFailed};
    }

    // 用户名来自外部输入，查询前必须先转义。
    const std::string escaped_username = EscapeSqlValue(conn.get(), username);
    const std::string query =
        "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
        "FROM users WHERE username='" +
        escaped_username + "' LIMIT 1";
    return ReadSingleUser(conn.get(), query);
  }

  FindUserResult UserRepository::findById(UserId user_id)
  {
    MysqlPtr conn = Connect(config_);
    if (conn == nullptr)
    {
      return {.status = RepositoryStatus::kQueryFailed};
    }

    const std::string query =
        "SELECT id, username, password_hash, nickname, status, created_at, updated_at "
        "FROM users WHERE id=" +
        std::to_string(user_id) + " LIMIT 1";
    return ReadSingleUser(conn.get(), query);
  }

  CreateUserResult UserRepository::createUser(const std::string &username,
                                              const std::string &password_hash,
                                              const std::string &nickname)
  {
    MysqlPtr conn = Connect(config_);
    if (conn == nullptr)
    {
      return {.status = RepositoryStatus::kInsertFailed};
    }

    // 插入语句涉及多个外部字符串字段，都需要分别转义。
    const std::string escaped_username = EscapeSqlValue(conn.get(), username);
    const std::string escaped_password_hash =
        EscapeSqlValue(conn.get(), password_hash);
    const std::string escaped_nickname = EscapeSqlValue(conn.get(), nickname);
    const std::string query =
        "INSERT INTO users(username, password_hash, nickname, status) VALUES('" +
        escaped_username + "','" + escaped_password_hash + "','" +
        escaped_nickname + "',1)";

    if (mysql_query(conn.get(), query.c_str()) != 0)
    {
      if (mysql_errno(conn.get()) == kMysqlDuplicateEntryError)
      {
        return {.status = RepositoryStatus::kDuplicate};
      }
      LogMysqlError(conn.get(), "insert user");
      return {.status = RepositoryStatus::kInsertFailed};
    }

    // users.id 使用自增主键，因此成功插入后直接读取 insert_id 作为 user_id。
    const UserId user_id = static_cast<UserId>(mysql_insert_id(conn.get()));
    if (user_id <= 0)
    {
      return {.status = RepositoryStatus::kInsertFailed};
    }
    return {.status = RepositoryStatus::kOk, .user_id = user_id};
  }

} // namespace chat
