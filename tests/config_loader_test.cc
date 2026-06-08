#include "config/config_loader.h"

#include <gtest/gtest.h>

#include "config/server_config.h"
#include "nlohmann/json.hpp"

// 本文件测试 ConfigLoader 的解析、校验和脱敏行为。

namespace chat {
namespace {

// ── 辅助函数 ──────────────────────────────────────────────────────────────────

const ServerConfig& OkConfig(const ConfigResult& result) {
    EXPECT_TRUE(std::holds_alternative<ServerConfig>(result))
        << "Expected OK but got error: "
        << (std::holds_alternative<ConfigError>(result) ? std::get<ConfigError>(result).message : "(unknown)");
    return std::get<ServerConfig>(result);
}

std::string ErrMsg(const ConfigResult& result) {
    if (std::holds_alternative<ConfigError>(result)) return std::get<ConfigError>(result).message;
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

TEST(ConfigLoaderTest, LoadFromString_MinimalValid) {
    auto result = ConfigLoader::LoadFromString(kMinimalValid);
    const auto& cfg = OkConfig(result);
    EXPECT_EQ(cfg.server.listen_ip, "127.0.0.1");
    EXPECT_EQ(cfg.server.listen_port, 8080);
    EXPECT_EQ(cfg.mysql.host, "127.0.0.1");
    EXPECT_EQ(cfg.mysql.username, "root");
    EXPECT_EQ(cfg.mysql.database, "chat");
    EXPECT_EQ(cfg.timeout.remote_push_ms, 500u);
}

TEST(ConfigLoaderTest, LoadFromString_DefaultsApplied) {
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
}

// ── JSON 错误 ─────────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, InvalidJson_ReturnsParseError) {
    auto result = ConfigLoader::LoadFromString("{invalid json}");
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("JSON parse error"), std::string::npos);
}

TEST(ConfigLoaderTest, RootNotObject_ReturnsError) {
    auto result = ConfigLoader::LoadFromString("[1, 2, 3]");
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("root"), std::string::npos);
}

// ── 字段类型错误 ──────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, TypeError_ListenPort) {
    const char* json = R"({
      "server": { "listen_ip": "127.0.0.1", "listen_port": "not_a_number" },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("listen_port"), std::string::npos);
}

TEST(ConfigLoaderTest, TypeError_MysqlHost) {
    const char* json = R"({
      "mysql": { "host": 123, "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("host"), std::string::npos);
}

TEST(ConfigLoaderTest, TypeError_PoolMaxConnections) {
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

TEST(ConfigLoaderTest, Validate_InvalidListenIp) {
    const char* json = R"({
      "server": { "listen_ip": "not_an_ip" },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("listen_ip"), std::string::npos);
}

TEST(ConfigLoaderTest, Validate_IPv6ListenIp) {
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

TEST(ConfigLoaderTest, Validate_MysqlEmptyHost) {
    const char* json = R"({
      "mysql": { "host": "", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.host"), std::string::npos);
}

TEST(ConfigLoaderTest, Validate_MysqlEmptyUsername) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.username"), std::string::npos);
}

TEST(ConfigLoaderTest, Validate_MysqlEmptyDatabase) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("mysql.database"), std::string::npos);
}

TEST(ConfigLoaderTest, Validate_PoolMaxZero) {
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

TEST(ConfigLoaderTest, Validate_PoolMinGtMax) {
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

TEST(ConfigLoaderTest, Validate_RemotePushMsZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "timeout": { "remote_push_ms": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("timeout.remote_push_ms"), std::string::npos);
}

TEST(ConfigLoaderTest, Validate_ConnectTimeoutZero) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d", "connect_timeout_seconds": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("connect_timeout_seconds"), std::string::npos);
}

// ── Redis 校验（仅 enabled=true 时生效）────────────────────────────────────

TEST(ConfigLoaderTest, Validate_RedisDisabledSkipsValidation) {
    // Redis disabled 时，仅跳过语义校验（host 非空、pool_size > 0 等），
    // 但端口值仍需合法（GetPort 在解析阶段校验）。
    // 这里传入 host 为空、pool_size 为 0 ——这些只在 enabled=true 时校验。
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "enabled": false, "host": "", "port": 6379, "pool_size": 0 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_FALSE(IsError(result)) << ErrMsg(result);
}

TEST(ConfigLoaderTest, Validate_RedisEnabledEmptyHost) {
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" },
      "redis": { "enabled": true, "host": "", "port": 6379, "pool_size": 4 }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result));
    EXPECT_NE(ErrMsg(result).find("redis.host"), std::string::npos);
}

// ── 脱敏输出 ──────────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, ToSafeString_RedactsPasswords) {
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

TEST(ConfigLoaderTest, ToSafeString_ValidJson) {
    auto result = ConfigLoader::LoadFromString(kMinimalValid);
    const auto& cfg = OkConfig(result);
    std::string safe = cfg.ToSafeString();
    // 能被 JSON 解析
    EXPECT_NO_THROW({ auto parsed = nlohmann::json::parse(safe); });
}

// ── 完整配置 JSON ─────────────────────────────────────────────────────────────

TEST(ConfigLoaderTest, FullConfig_AllFieldsParsed) {
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
        "connect_timeout_ms": 1000, "key_prefix": "myapp:",
        "session_ttl_seconds": 3600, "rate_limit_window_seconds": 30,
        "rate_limit_max_requests": 50
      },
      "log": { "level": "debug", "path": "/var/log/server.log", "console": false },
      "timeout": { "remote_push_ms": 1000 }
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
    EXPECT_EQ(cfg.redis.key_prefix, "myapp:");
    EXPECT_EQ(cfg.redis.session_ttl_seconds, 3600);
    EXPECT_EQ(cfg.redis.rate_limit_window_seconds, 30);
    EXPECT_EQ(cfg.redis.rate_limit_max_requests, 50);
    EXPECT_EQ(cfg.log.level, "debug");
    EXPECT_EQ(cfg.log.path, "/var/log/server.log");
    EXPECT_FALSE(cfg.log.console);
    EXPECT_EQ(cfg.timeout.remote_push_ms, 1000u);
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
TEST(ConfigLoaderTest, P1_PortOverflow_ServerListenPort) {
    const char* json = R"({
      "server": { "listen_ip": "127.0.0.1", "listen_port": 65537 },
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ASSERT_TRUE(IsError(result)) << "Expected error for port 65537, got OK";
    EXPECT_NE(ErrMsg(result).find("listen_port"), std::string::npos) << ErrMsg(result);
}

TEST(ConfigLoaderTest, P1_PortOverflow_MysqlPort) {
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

TEST(ConfigLoaderTest, P1_PortOverflow_RedisPort) {
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
TEST(ConfigLoaderTest, P1_BadEnvPort_ReturnsError) {
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

TEST(ConfigLoaderTest, P1_BadEnvPort_Overflow_ReturnsError) {
    ::setenv("CHAT_DB_PORT", "65537", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_DB_PORT");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_DB_PORT=65537";
    EXPECT_NE(ErrMsg(result).find("CHAT_DB_PORT"), std::string::npos) << ErrMsg(result);
}

TEST(ConfigLoaderTest, P1_BadEnvRedisPort_ReturnsError) {
    ::setenv("CHAT_REDIS_PORT", "not_a_port", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_REDIS_PORT");

    ASSERT_TRUE(IsError(result)) << "Expected error for CHAT_REDIS_PORT=not_a_port";
    EXPECT_NE(ErrMsg(result).find("CHAT_REDIS_PORT"), std::string::npos) << ErrMsg(result);
}

TEST(ConfigLoaderTest, P1_BadEnvRedisDb_ReturnsError) {
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
TEST(ConfigLoaderTest, P1_ValidEnvPort_Accepted) {
    ::setenv("CHAT_DB_PORT", "5432", 1);
    const char* json = R"({
      "mysql": { "host": "h", "username": "u", "database": "d" }
    })";
    auto result = ConfigLoader::LoadFromString(json);
    ::unsetenv("CHAT_DB_PORT");

    ASSERT_FALSE(IsError(result)) << ErrMsg(result);
    EXPECT_EQ(std::get<ServerConfig>(result).mysql.port, 5432);
}

}  // namespace
}  // namespace chat
