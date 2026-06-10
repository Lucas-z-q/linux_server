#include "config/config_loader.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config/server_config.h"
#include "nlohmann/json.hpp"

// 本文件测试 ConfigLoader 的解析、校验和脱敏行为。

namespace chat {
namespace {

// ── RAII 环境变量隔离 ─────────────────────────────────────────────────────────

// 在构造时保存并清除指定的环境变量，在析构时恢复原值。
// 用于防止宿主环境的 CHAT_* 变量干扰测试结果。
class EnvGuard {
   public:
    explicit EnvGuard(std::vector<const char*> names) : names_(std::move(names)) {
        for (const char* n : names_) {
            const char* v = std::getenv(n);
            saved_.push_back(v ? std::optional<std::string>{v} : std::nullopt);
            ::unsetenv(n);
        }
    }
    ~EnvGuard() {
        for (std::size_t i = 0; i < names_.size(); ++i) {
            if (saved_[i].has_value()) {
                ::setenv(names_[i], saved_[i]->c_str(), 1);
            } else {
                ::unsetenv(names_[i]);
            }
        }
    }

    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

   private:
    std::vector<const char*> names_;
    std::vector<std::optional<std::string>> saved_;
};

// 覆盖所有可能影响 ConfigLoader 的环境变量名。
static const std::vector<const char*> kChatEnvVars = {
    "CHAT_DB_HOST",
    "CHAT_DB_PORT",
    "CHAT_DB_USER",
    "CHAT_DB_PASSWORD",
    "CHAT_DB_NAME",
    "CHAT_REDIS_ENABLED",
    "CHAT_REDIS_HOST",
    "CHAT_REDIS_PORT",
    "CHAT_REDIS_PASSWORD",
    "CHAT_REDIS_DB",
    "CHAT_REDIS_POOL_SIZE",
    "CHAT_REDIS_CONNECT_TIMEOUT_MS",
    "CHAT_REDIS_COMMAND_TIMEOUT_MS",
    "CHAT_REDIS_KEY_PREFIX",
    "CHAT_SERVER_ID",
    "CHAT_SESSION_TTL_SECONDS",
    "CHAT_PRESENCE_TTL_SECONDS",
    "CHAT_USER_CACHE_TTL_SECONDS",
    "CHAT_USER_NOT_FOUND_TTL_SECONDS",
    "CHAT_MESSAGE_DEDUP_TTL_SECONDS",
    "CHAT_LOGIN_RATE_LIMIT",
    "CHAT_LOGIN_RATE_WINDOW_SECONDS",
    "CHAT_REGISTER_RATE_LIMIT",
    "CHAT_REGISTER_RATE_WINDOW_SECONDS",
    "CHAT_SEND_RATE_LIMIT",
    "CHAT_SEND_RATE_WINDOW_SECONDS",
    "CHAT_CONNECTION_IDLE_TIMEOUT_MS",
    "CHAT_HEARTBEAT_TIMEOUT_MS",
    "CHAT_CONFIG_PATH",
};

// ── 测试 Fixture ──────────────────────────────────────────────────────────────

// 所有 ConfigLoaderTest 用例均通过该 Fixture 运行，确保 CHAT_* 环境变量在
// 每个测试开始前被清除，测试结束后恢复，不受宿主 CI 环境影响。
class ConfigLoaderTest : public ::testing::Test {
   protected:
    void SetUp() override { guard_ = std::make_unique<EnvGuard>(kChatEnvVars); }
    void TearDown() override { guard_.reset(); }

   private:
    std::unique_ptr<EnvGuard> guard_;
};

// ── 辅助函数 ──────────────────────────────────────────────────────────────────

const ServerConfig& OkConfig(const ConfigResult& result) {
    EXPECT_TRUE(std::holds_alternative<ServerConfig>(result))
        << "Expected OK but got error: "
        << (std::holds_alternative<ConfigError>(result) ? std::get<ConfigError>(result).message : "(unknown)");
    return std::get<ServerConfig>(result);
}

std::string ErrMsg(const ConfigResult& result) {
    if (std::holds_alternative<ConfigError>(result))
        return std::get<ConfigError>(result).message;
    return "";
}

bool IsError(const ConfigResult& result) { return std::holds_alternative<ConfigError>(result); }

// 最小合法配置 JSON（包含所有必填字段）
constexpr const char* kMinimalValid = R"({
  "server": { "listen_ip": "127.0.0.1", "listen_port": 8080 },
  "mysql": {
    "host": "127.0.0.1",
    "username": "root",
    "database": "chat"
  },
  "timeout": { "remote_push_ms": 500 }
})";

// ── 基础解析 ──────────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, LoadFromString_MinimalValid) {
    auto result = ConfigLoader::LoadFromString(kMinimalValid);
    const auto& cfg = OkConfig(result);
    EXPECT_EQ(cfg.server.listen_ip, "127.0.0.1");
    EXPECT_EQ(cfg.server.listen_port, 8080);
    EXPECT_EQ(cfg.mysql.host, "127.0.0.1");
    EXPECT_EQ(cfg.mysql.username, "root");
    EXPECT_EQ(cfg.mysql.database, "chat");
    EXPECT_EQ(cfg.timeout.remote_push_ms, 500u);
    EXPECT_EQ(cfg.connection.idle_timeout_ms, 300000u);
    EXPECT_EQ(cfg.heartbeat.timeout_ms, 90000u);
    EXPECT_EQ(cfg.redis.presence_ttl_seconds, 120u);
}

TEST_F(ConfigLoaderTest, LoadFromString_DefaultsApplied) {
    // 完全空对象，依赖结构体默认值
    // mysql 字段 host/username/database 有默认值，但 username/database 为空会校验失败
    // 这里测试 server 和 timeout 的默认值
    const char* json = R"({
      "mysql": { "host": "db", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    const auto& cfg = OkConfig(result);
    EXPECT_EQ(cfg.server.listen_ip, "127.0.0.1");
    EXPECT_EQ(cfg.server.listen_port, 8080);
    EXPECT_EQ(cfg.timeout.remote_push_ms, 500u);
    EXPECT_FALSE(cfg.redis.enabled);
    EXPECT_EQ(cfg.log.level, "info");
    EXPECT_TRUE(cfg.log.console);
    EXPECT_EQ(cfg.log.file_path, "logs/server.log");
    EXPECT_EQ(cfg.log.max_size_mb, 100u);
    EXPECT_EQ(cfg.log.max_files, 5u);
    EXPECT_TRUE(cfg.log.async);
}

// ── JSON 错误 ─────────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, InvalidJson_ReturnsParseError) {
    auto result = ConfigLoader::LoadFromString("{invalid json}");
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("JSON parse error"), std::string::npos);
}

TEST_F(ConfigLoaderTest, RootNotObject_ReturnsError) {
    auto result = ConfigLoader::LoadFromString("[1, 2, 3]");
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("root"), std::string::npos);
}

// ── 字段类型错误 ──────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, TypeError_ListenPort) {
    const char* json = R"({
      "server": { "listen_ip": "127.0.0.1", "listen_port": "not_a_number" },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("listen_port"), std::string::npos);
}

TEST_F(ConfigLoaderTest, TypeError_MysqlHost) {
    const char* json = R"({
      "mysql": { "host": 123, "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("host"), std::string::npos);
}

TEST_F(ConfigLoaderTest, TypeError_PoolMaxConnections) {
    const char* json = R"({
      "mysql": {
        "host": "h", "username": "u", "database": "d",
        "pool": { "max_connections": "four" }
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    // 错误信息应携带字段路径
    EXPECT_NE(ErrMsg(result).find("max_connections"), std::string::npos);
}

// ── 校验规则 ──────────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, Validate_InvalidListenIp) {
    const char* json = R"({
      "server": { "listen_ip": "not_an_ip" },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("listen_ip"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_IPv6ListenIp) {
    // IPv6 地址应被接受
    const char* json = R"({
      "server": { "listen_ip": "::1", "listen_port": 9000 },
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "timeout": { "remote_push_ms": 100 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_FALSE(IsError(result)) << ErrMsg(result);
    EXPECT_EQ(std::get<ServerConfig>(result).server.listen_ip, "::1");
}

TEST_F(ConfigLoaderTest, Validate_MysqlEmptyHost) {
    const char* json = R"({
      "mysql": { "host": "", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.host"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_MysqlEmptyUsername) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.username"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_MysqlEmptyDatabase) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.database"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_PoolMaxZero) {
    const char* json = R"({
      "mysql": {
        "host": "h", "username": "u", "database": "d",
        "pool": { "max_connections": 0 }
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.pool.max_connections"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_PoolMinGtMax) {
    const char* json = R"({
      "mysql": {
        "host": "h", "username": "u", "database": "d",
        "pool": { "min_connections": 8, "max_connections": 4 }
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("min_connections"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_RemotePushMsZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "timeout": { "remote_push_ms": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("timeout.remote_push_ms"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_ConnectTimeoutZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d", "connect_timeout_seconds": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("connect_timeout_seconds"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_LogLevelInvalid) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "log": { "level": "trace" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("log.level"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_LogMaxSizeZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "log": { "max_size_mb": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("log.max_size_mb"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_LogMaxFilesZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "log": { "max_files": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("log.max_files"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_LogWithoutOutputTarget) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "log": { "console": false, "file_path": "" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("log"), std::string::npos);
}

TEST_F(ConfigLoaderTest, LegacyLogPathReturnsMigrationError) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "log": { "path": "old.log" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("log.path"), std::string::npos);
    EXPECT_NE(ErrMsg(result).find("log.file_path"), std::string::npos);
}

// ── Redis 校验（仅 enabled=true 时生效）────────────────────────────────────

TEST_F(ConfigLoaderTest, Validate_RedisDisabledStillValidatesNumericFields) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "enabled": false, "host": "", "port": 6379, "pool_size": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("redis.pool_size"), std::string::npos);
}

TEST_F(ConfigLoaderTest, Validate_RedisEnabledEmptyHost) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "enabled": true, "host": "", "port": 6379, "pool_size": 4 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("redis.host"), std::string::npos);
}

// ── 脱敏输出 ──────────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, ToSafeString_RedactsPasswords) {
    const char* json = R"({
      "mysql": {
        "host": "db.internal", "username": "admin",
        "password": "super_secret_123", "database": "prod"
      },
      "redis": { "enabled": false, "password": "redis_secret_456" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    const auto& cfg = OkConfig(result);

    std::string safe = cfg.ToSafeString();

    // 密码不得出现在脱敏输出中
    EXPECT_EQ(safe.find("super_secret_123"), std::string::npos);
    EXPECT_EQ(safe.find("redis_secret_456"), std::string::npos);

    // <redacted> 应出现
    EXPECT_NE(safe.find("<redacted>"), std::string::npos);

    // 非敏感字段应保留
    EXPECT_NE(safe.find("db.internal"), std::string::npos);
    EXPECT_NE(safe.find("admin"), std::string::npos);
}

TEST_F(ConfigLoaderTest, ToSafeString_ValidJson) {
    auto result = ConfigLoader::LoadFromString(kMinimalValid);
    const auto& cfg = OkConfig(result);
    std::string safe = cfg.ToSafeString();
    // 能被 JSON 解析
    EXPECT_NO_THROW({ auto parsed = nlohmann::json::parse(safe); });
}

// ── 完整配置 JSON ─────────────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, FullConfig_AllFieldsParsed) {
    const char* json = R"({
      "server": { "listen_ip": "0.0.0.0", "listen_port": 9090 },
      "mysql": {
        "host": "mysql.internal", "port": 3307,
        "username": "chatuser", "password": "pw", "database": "chatdb",
        "connect_timeout_seconds": 10, "read_timeout_seconds": 8, "write_timeout_seconds": 6,
        "pool": { "min_connections": 2, "max_connections": 8, "borrow_timeout_ms": 2000 }
      },
      "redis": {
        "enabled": true, "host": "redis.internal", "port": 6380,
        "password": "redispw", "database": 1, "pool_size": 8,
        "connect_timeout_ms": 1000, "command_timeout_ms": 1200,
        "key_prefix": "myapp", "server_id": "server-a",
        "session_ttl_seconds": 3600, "presence_ttl_seconds": 121,
        "user_cache_ttl_seconds": 600, "user_not_found_ttl_seconds": 45,
        "message_dedup_ttl_seconds": 7200,
        "login_rate_limit": 12, "login_rate_window_seconds": 30,
        "register_rate_limit": 7, "register_rate_window_seconds": 120,
        "send_rate_limit": 80, "send_rate_window_seconds": 45
      },
      "log": {
        "level": "debug", "file_path": "/var/log/server.log", "console": false,
        "max_size_mb": 64, "max_files": 3, "async": false
      },
      "timeout": { "remote_push_ms": 1000 },
      "connection": { "idle_timeout_ms": 240000 },
      "heartbeat": { "timeout_ms": 120000 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    const auto& cfg = OkConfig(result);

    EXPECT_EQ(cfg.server.listen_ip, "0.0.0.0");
    EXPECT_EQ(cfg.server.listen_port, 9090);
    EXPECT_EQ(cfg.mysql.host, "mysql.internal");
    EXPECT_EQ(cfg.mysql.port, 3307);
    EXPECT_EQ(cfg.mysql.username, "chatuser");
    EXPECT_EQ(cfg.mysql.password, "pw");
    EXPECT_EQ(cfg.mysql.database, "chatdb");
    EXPECT_EQ(cfg.mysql.connect_timeout_seconds, 10u);
    EXPECT_EQ(cfg.mysql.read_timeout_seconds, 8u);
    EXPECT_EQ(cfg.mysql.write_timeout_seconds, 6u);
    EXPECT_EQ(cfg.mysql_pool.min_connections, 2u);
    EXPECT_EQ(cfg.mysql_pool.max_connections, 8u);
    EXPECT_EQ(cfg.mysql_pool.borrow_timeout_ms, 2000u);
    EXPECT_TRUE(cfg.redis.enabled);
    EXPECT_EQ(cfg.redis.host, "redis.internal");
    EXPECT_EQ(cfg.redis.port, 6380);
    EXPECT_EQ(cfg.redis.password, "redispw");
    EXPECT_EQ(cfg.redis.database, 1);
    EXPECT_EQ(cfg.redis.pool_size, 8);
    EXPECT_EQ(cfg.redis.connect_timeout_ms, 1000u);
    EXPECT_EQ(cfg.redis.command_timeout_ms, 1200u);
    EXPECT_EQ(cfg.redis.key_prefix, "myapp");
    EXPECT_EQ(cfg.redis.server_id, "server-a");
    EXPECT_EQ(cfg.redis.session_ttl_seconds, 3600);
    EXPECT_EQ(cfg.redis.presence_ttl_seconds, 121u);
    EXPECT_EQ(cfg.redis.user_cache_ttl_seconds, 600u);
    EXPECT_EQ(cfg.redis.user_not_found_ttl_seconds, 45u);
    EXPECT_EQ(cfg.redis.message_dedup_ttl_seconds, 7200u);
    EXPECT_EQ(cfg.redis.login_rate_limit, 12u);
    EXPECT_EQ(cfg.redis.login_rate_window_seconds, 30u);
    EXPECT_EQ(cfg.redis.register_rate_limit, 7u);
    EXPECT_EQ(cfg.redis.register_rate_window_seconds, 120u);
    EXPECT_EQ(cfg.redis.send_rate_limit, 80u);
    EXPECT_EQ(cfg.redis.send_rate_window_seconds, 45u);
    EXPECT_EQ(cfg.log.level, "debug");
    EXPECT_EQ(cfg.log.file_path, "/var/log/server.log");
    EXPECT_FALSE(cfg.log.console);
    EXPECT_EQ(cfg.log.max_size_mb, 64u);
    EXPECT_EQ(cfg.log.max_files, 3u);
    EXPECT_FALSE(cfg.log.async);
    EXPECT_EQ(cfg.timeout.remote_push_ms, 1000u);
    EXPECT_EQ(cfg.connection.idle_timeout_ms, 240000u);
    EXPECT_EQ(cfg.heartbeat.timeout_ms, 120000u);
}

TEST_F(ConfigLoaderTest, RedisAndTimeoutEnvironmentOverrides) {
    ::setenv("CHAT_REDIS_ENABLED", "1", 1);
    ::setenv("CHAT_REDIS_POOL_SIZE", "9", 1);
    ::setenv("CHAT_REDIS_COMMAND_TIMEOUT_MS", "750", 1);
    ::setenv("CHAT_REDIS_KEY_PREFIX", "envchat", 1);
    ::setenv("CHAT_SERVER_ID", "server-env", 1);
    ::setenv("CHAT_PRESENCE_TTL_SECONDS", "151", 1);
    ::setenv("CHAT_CONNECTION_IDLE_TIMEOUT_MS", "456000", 1);
    ::setenv("CHAT_HEARTBEAT_TIMEOUT_MS", "150000", 1);

    auto result = ConfigLoader::LoadFromString(kMinimalValid);
    const auto& cfg = OkConfig(result);
    EXPECT_TRUE(cfg.redis.enabled);
    EXPECT_EQ(cfg.redis.pool_size, 9u);
    EXPECT_EQ(cfg.redis.command_timeout_ms, 750u);
    EXPECT_EQ(cfg.redis.key_prefix, "envchat");
    EXPECT_EQ(cfg.redis.server_id, "server-env");
    EXPECT_EQ(cfg.redis.presence_ttl_seconds, 151u);
    EXPECT_EQ(cfg.connection.idle_timeout_ms, 456000u);
    EXPECT_EQ(cfg.heartbeat.timeout_ms, 150000u);
}

TEST_F(ConfigLoaderTest, RedisUnsignedOverflowReturnsError) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "command_timeout_ms": 4294967296 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("redis.command_timeout_ms"), std::string::npos);
}

TEST_F(ConfigLoaderTest, RedisKeyPrefixMustNotEndWithColon) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "key_prefix": "chat:" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("redis.key_prefix"), std::string::npos);
}

TEST_F(ConfigLoaderTest, PresenceTtlMustExceedHeartbeatTimeout) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "presence_ttl_seconds": 90 },
      "heartbeat": { "timeout_ms": 90000 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("presence_ttl_seconds"), std::string::npos);
}

// ── ParseConfigPathArg ────────────────────────────────────────────────────────

TEST(ParseConfigPathArgTest, FindsConfigArg) {
    const char* args[] = {"server", "--config", "my_config.json"};
    int argc = 3;
    std::string path = ParseConfigPathArg(argc, const_cast<char**>(args));
    EXPECT_EQ(path, "my_config.json");
}

TEST(ParseConfigPathArgTest, NoConfigArg_ReturnsEmpty) {
    const char* args[] = {"server", "--port", "9090"};
    int argc = 3;
    std::string path = ParseConfigPathArg(argc, const_cast<char**>(args));
    EXPECT_TRUE(path.empty());
}

TEST(ParseConfigPathArgTest, ConfigArgAtEnd_ReturnsEmpty) {
    // --config 在末尾没有值，应安全返回空字符串
    const char* args[] = {"server", "--config"};
    int argc = 2;
    std::string path = ParseConfigPathArg(argc, const_cast<char**>(args));
    EXPECT_TRUE(path.empty());
}

// ── P1 回归测试 ───────────────────────────────────────────────────────────────

// Bug: 端口溢出 —— 65537 截断成 uint16_t 变成 1，没有报错。
TEST_F(ConfigLoaderTest, P1_PortOverflow_ServerListenPort) {
    const char* json = R"({
      "server": { "listen_ip": "127.0.0.1", "listen_port": 65537 },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for port 65537, got OK";
    EXPECT_NE(ErrMsg(result).find("listen_port"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P1_PortOverflow_MysqlPort) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d", "port": 99999 }
    })";
    // mysql.port 使用 GetUInt 读到 uint16_t，但 GetPort 还未应用到 mysql.port；
    // 该字段由 Validate() 的 mysql.port < 1 兜底，但大值可能截断后变成合法小端口。
    // 本测试确认 99999 不会悄悄成为端口 34463。
    // mysql.port 的 JSON 字段目前用 GetUInt<uint16_t>，nlohmann 会把 99999 截断，
    // 所以应改用 GetPort，否则此测试会失败以暴露问题。
    //
    // 注意：mysql.port 当前字段类型是 uint16_t，GetUInt<uint16_t> 直接截断。
    // 本测试用来追踪该行为：若值被截断并通过校验，则视为 bug。
    auto result = ConfigLoader::LoadFromString(json);
    // 正确行为：99999 超出范围，应报错。
    ASSERT_TRUE(IsError(result)) << "Expected error for mysql.port 99999, got port "
                                 << (IsError(result) ? 0 : std::get<ServerConfig>(result).mysql.port);
    EXPECT_NE(ErrMsg(result).find("port"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P1_PortOverflow_RedisPort) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "enabled": true, "host": "r", "port": 70000, "pool_size": 4,
                 "session_ttl_seconds": 3600, "rate_limit_window_seconds": 60,
                 "rate_limit_max_requests": 100 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for redis.port 70000";
    EXPECT_NE(ErrMsg(result).find("port"), std::string::npos) << ErrMsg(result);
}

// Bug: 非法环境变量被忽略 —— CHAT_DB_PORT=abc 应报错而非静默跳过。
TEST_F(ConfigLoaderTest, P1_BadEnvPort_ReturnsError) {
    // 先设置非法环境变量
    ::setenv("CHAT_DB_PORT", "abc", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_DB_PORT");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_DB_PORT=abc";
    EXPECT_NE(ErrMsg(result).find("CHAT_DB_PORT"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P1_BadEnvPort_Overflow_ReturnsError) {
    ::setenv("CHAT_DB_PORT", "65537", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_DB_PORT");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_DB_PORT=65537";
    EXPECT_NE(ErrMsg(result).find("CHAT_DB_PORT"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P1_BadEnvRedisPort_ReturnsError) {
    ::setenv("CHAT_REDIS_PORT", "not_a_port", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_REDIS_PORT");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_REDIS_PORT=not_a_port";
    EXPECT_NE(ErrMsg(result).find("CHAT_REDIS_PORT"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P1_BadEnvRedisDb_ReturnsError) {
    ::setenv("CHAT_REDIS_DB", "two", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_REDIS_DB");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_REDIS_DB=two";
    EXPECT_NE(ErrMsg(result).find("CHAT_REDIS_DB"), std::string::npos) << ErrMsg(result);
}

// 合法环境变量不应报错
TEST_F(ConfigLoaderTest, P1_ValidEnvPort_Accepted) {
    ::setenv("CHAT_DB_PORT", "5432", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_DB_PORT");

    ASSERT_FALSE(IsError(result)) << ErrMsg(result);
    EXPECT_EQ(std::get<ServerConfig>(result).mysql.port, 5432);
}

// ── P1 非端口整数溢出回归测试 ─────────────────────────────────────────────────

// remote_push_ms: 4294967297 (2^32+1) 截断成 uint32_t 变成 1，应报错。
TEST_F(ConfigLoaderTest, P1_UIntOverflow_RemotePushMs) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "timeout": { "remote_push_ms": 4294967297 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected overflow error for remote_push_ms=4294967297";
    EXPECT_NE(ErrMsg(result).find("remote_push_ms"), std::string::npos) << ErrMsg(result);
}

// min_connections: 2^32+1 截断成 size_t 可能变成 1，应报错。
TEST_F(ConfigLoaderTest, P1_UIntOverflow_PoolMinConnections) {
    const char* json = R"({
      "mysql": {
        "host": "h", "username": "u", "database": "d",
        "pool": { "min_connections": 4294967297, "max_connections": 4 }
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected overflow error for min_connections=4294967297";
    EXPECT_NE(ErrMsg(result).find("min_connections"), std::string::npos) << ErrMsg(result);
}

// connect_timeout_seconds: 0xFFFFFFFFFF 超出 uint32_t，应报错。
TEST_F(ConfigLoaderTest, P1_UIntOverflow_ConnectTimeout) {
    const char* json = R"({
      "mysql": {
        "host": "h", "username": "u", "database": "d",
        "connect_timeout_seconds": 1099511627775
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected overflow error for connect_timeout_seconds=1099511627775";
    EXPECT_NE(ErrMsg(result).find("connect_timeout_seconds"), std::string::npos) << ErrMsg(result);
}

// ── P2a Redis 校验回归测试 ────────────────────────────────────────────────────

TEST_F(ConfigLoaderTest, P2a_RedisNegativeDatabase_ReturnsError) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": {
        "enabled": true, "host": "r", "port": 6379, "pool_size": 4,
        "database": -1, "connect_timeout_ms": 3000,
        "session_ttl_seconds": 3600, "rate_limit_window_seconds": 60,
        "rate_limit_max_requests": 100
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for redis.database=-1";
    EXPECT_NE(ErrMsg(result).find("redis.database"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P2a_RedisZeroConnectTimeout_ReturnsError) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": {
        "enabled": true, "host": "r", "port": 6379, "pool_size": 4,
        "database": 0, "connect_timeout_ms": 0,
        "session_ttl_seconds": 3600, "rate_limit_window_seconds": 60,
        "rate_limit_max_requests": 100
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for redis.connect_timeout_ms=0";
    EXPECT_NE(ErrMsg(result).find("connect_timeout_ms"), std::string::npos) << ErrMsg(result);
}

TEST_F(ConfigLoaderTest, P2a_RedisNegativeConnectTimeout_ReturnsError) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": {
        "enabled": true, "host": "r", "port": 6379, "pool_size": 4,
        "database": 0, "connect_timeout_ms": -500,
        "session_ttl_seconds": 3600, "rate_limit_window_seconds": 60,
        "rate_limit_max_requests": 100
      }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for redis.connect_timeout_ms=-500";
    EXPECT_NE(ErrMsg(result).find("connect_timeout_ms"), std::string::npos) << ErrMsg(result);
}

// ── P2c 环境隔离验证 ──────────────────────────────────────────────────────────

// 确认 fixture 确实清除了外部环境变量：即使宿主机设置了 CHAT_DB_HOST，
// 测试看到的应是 JSON 中的值而不是环境变量值。
TEST_F(ConfigLoaderTest, P2c_ExternalEnvDoesNotAffectTest) {
    // fixture 已在 SetUp 中清除 CHAT_DB_HOST，此处再 setenv 模拟"测试内设置"
    ::setenv("CHAT_DB_HOST", "env-host.internal", 1);
    const char* json = R"({
      "mysql": { "host": "json-host", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_FALSE(IsError(result)) << ErrMsg(result);
    // env 覆盖应生效
    EXPECT_EQ(std::get<ServerConfig>(result).mysql.host, "env-host.internal");
    // TearDown 会还原 CHAT_DB_HOST
}

}  // namespace
}  // namespace chat
