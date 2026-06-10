#include "config/config_loader.h"

#include <arpa/inet.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "nlohmann/json.hpp"

// 本文件实现 ConfigLoader：
//   1. 自动探测配置文件路径
//   2. 从 JSON 填充 ServerConfig（先默认值 -> 再 JSON -> 再环境变量）
//   3. 全量校验并返回带字段路径的错误信息

namespace chat {

namespace {

// 严格整型转换，替代 atoi()，解析失败时返回 false。
template <typename T>
bool StrictParseInt(const char* str, T& out) {
    if (!str || *str == '\0')
        return false;
    auto [ptr, ec] = std::from_chars(str, str + std::strlen(str), out);
    return ec == std::errc{} && ptr == str + std::strlen(str);
}

// 读取环境变量，不存在时返回 nullptr。
const char* Env(const char* name) { return std::getenv(name); }

// 从文件读取全部内容；失败时返回空字符串并设 err_msg。
std::string ReadFile(const std::string& path, std::string& err_msg) {
    std::ifstream file(path);
    if (!file.is_open()) {
        err_msg = "cannot open config file: " + path;
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    if (file.bad()) {
        err_msg = "error reading config file: " + path;
        return "";
    }
    return ss.str();
}

// 从 JSON 对象安全取字符串字段；类型不匹配时设 err 并返回 false。
bool GetStr(const nlohmann::json& obj, const char* key, std::string& out, std::string& err) {
    if (!obj.contains(key))
        return true;
    if (!obj[key].is_string()) {
        err = std::string(key) + " must be a string";
        return false;
    }
    out = obj[key].get<std::string>();
    return true;
}

// 从 JSON 对象安全取布尔字段；类型不匹配时设 err 并返回 false。
bool GetBool(const nlohmann::json& obj, const char* key, bool& out, std::string& err) {
    if (!obj.contains(key))
        return true;
    if (!obj[key].is_boolean()) {
        err = std::string(key) + " must be a boolean";
        return false;
    }
    out = obj[key].get<bool>();
    return true;
}

// 从 JSON 对象安全取无符号整型字段并校验范围；类型不匹配或值超出 T 的表示范围时设 err 并返回 false。
template <typename T>
bool GetUInt(const nlohmann::json& obj, const char* key, T& out, const std::string& field_path, std::string& err) {
    if (!obj.contains(key))
        return true;
    if (!obj[key].is_number_unsigned()) {
        err = field_path + " must be an unsigned integer";
        return false;
    }
    // 先以 uint64_t 读取，再校验是否能装进目标类型，避免静默截断。
    uint64_t raw = obj[key].get<uint64_t>();
    if (raw > static_cast<uint64_t>(std::numeric_limits<T>::max())) {
        err = field_path + " value " + std::to_string(raw) + " overflows (max " +
              std::to_string(std::numeric_limits<T>::max()) + ")";
        return false;
    }
    out = static_cast<T>(raw);
    return true;
}

// 从 JSON 对象安全取有符号整型字段并校验范围；类型不匹配或值超出 T 的表示范围时设 err 并返回 false。
template <typename T>
bool GetInt(const nlohmann::json& obj, const char* key, T& out, const std::string& field_path, std::string& err) {
    if (!obj.contains(key))
        return true;
    if (!obj[key].is_number_integer()) {
        err = field_path + " must be an integer";
        return false;
    }
    // 先以 int64_t 读取，再校验是否能装进目标类型，避免静默截断。
    int64_t raw = obj[key].get<int64_t>();
    if (raw < static_cast<int64_t>(std::numeric_limits<T>::min()) ||
        raw > static_cast<int64_t>(std::numeric_limits<T>::max())) {
        err = field_path + " value " + std::to_string(raw) + " out of range [" +
              std::to_string(std::numeric_limits<T>::min()) + ", " + std::to_string(std::numeric_limits<T>::max()) +
              "]";
        return false;
    }
    out = static_cast<T>(raw);
    return true;
}

// 从 JSON 对象读取端口字段（uint32_t），并在赋值前严格校验范围 1..65535。
// 这样可以在截断成 uint16_t 之前捕获 65537 之类的溢出值。
bool GetPort(const nlohmann::json& obj, const char* key, uint16_t& out, const std::string& field_path,
             std::string& err) {
    if (!obj.contains(key))
        return true;
    if (!obj[key].is_number_unsigned()) {
        err = field_path + " must be an unsigned integer";
        return false;
    }
    uint64_t raw = obj[key].get<uint64_t>();
    if (raw < 1 || raw > 65535) {
        err = field_path + " must be in range 1..65535 (got " + std::to_string(raw) + ")";
        return false;
    }
    out = static_cast<uint16_t>(raw);
    return true;
}

// 解析 "server" 段。
std::string ParseServer(const nlohmann::json& root, ServerSection& cfg) {
    if (!root.contains("server"))
        return "";
    const auto& s = root["server"];
    if (!s.is_object())
        return "server must be an object";
    std::string err;
    if (!GetStr(s, "listen_ip", cfg.listen_ip, err))
        return "server." + err;
    // 使用 GetPort 在截断前校验范围，防止 65537 -> 1 的静默溢出。
    if (!GetPort(s, "listen_port", cfg.listen_port, "server.listen_port", err))
        return err;
    return "";
}

// 解析 "mysql" 段。
std::string ParseMysql(const nlohmann::json& root, DbConfig& cfg, DbPoolConfig& pool) {
    if (!root.contains("mysql"))
        return "";
    const auto& m = root["mysql"];
    if (!m.is_object())
        return "mysql must be an object";
    std::string err;

    if (!GetStr(m, "host", cfg.host, err))
        return "mysql." + err;
    if (!GetStr(m, "username", cfg.username, err))
        return "mysql." + err;
    if (!GetStr(m, "password", cfg.password, err))
        return "mysql." + err;
    if (!GetStr(m, "database", cfg.database, err))
        return "mysql." + err;
    // 使用 GetPort 在截断前校验范围，防止 99999 之类的值静默成为合法端口。
    if (!GetPort(m, "port", cfg.port, "mysql.port", err))
        return err;
    if (!GetUInt(m, "connect_timeout_seconds", cfg.connect_timeout_seconds, "mysql.connect_timeout_seconds", err))
        return err;
    if (!GetUInt(m, "read_timeout_seconds", cfg.read_timeout_seconds, "mysql.read_timeout_seconds", err))
        return err;
    if (!GetUInt(m, "write_timeout_seconds", cfg.write_timeout_seconds, "mysql.write_timeout_seconds", err))
        return err;

    if (!m.contains("pool"))
        return "";
    const auto& p = m["pool"];
    if (!p.is_object())
        return "mysql.pool must be an object";
    if (!GetUInt(p, "min_connections", pool.min_connections, "mysql.pool.min_connections", err))
        return err;
    if (!GetUInt(p, "max_connections", pool.max_connections, "mysql.pool.max_connections", err))
        return err;
    if (!GetUInt(p, "borrow_timeout_ms", pool.borrow_timeout_ms, "mysql.pool.borrow_timeout_ms", err))
        return err;
    return "";
}

// 解析 "redis" 段。
std::string ParseRedis(const nlohmann::json& root, RedisConfig& cfg) {
    if (!root.contains("redis"))
        return "";
    const auto& r = root["redis"];
    if (!r.is_object())
        return "redis must be an object";
    std::string err;

    if (!GetBool(r, "enabled", cfg.enabled, err))
        return "redis." + err;
    if (!GetStr(r, "host", cfg.host, err))
        return "redis." + err;
    if (!GetStr(r, "password", cfg.password, err))
        return "redis." + err;
    if (!GetStr(r, "key_prefix", cfg.key_prefix, err))
        return "redis." + err;

    // 使用 GetPort 在截断前校验范围。
    if (!GetPort(r, "port", cfg.port, "redis.port", err))
        return err;
    if (!GetInt(r, "database", cfg.database, "redis.database", err))
        return err;
    if (!GetInt(r, "pool_size", cfg.pool_size, "redis.pool_size", err))
        return err;
    if (!GetInt(r, "connect_timeout_ms", cfg.connect_timeout_ms, "redis.connect_timeout_ms", err))
        return err;
    if (!GetInt(r, "session_ttl_seconds", cfg.session_ttl_seconds, "redis.session_ttl_seconds", err))
        return err;
    if (!GetInt(r, "rate_limit_window_seconds", cfg.rate_limit_window_seconds, "redis.rate_limit_window_seconds", err))
        return err;
    if (!GetInt(r, "rate_limit_max_requests", cfg.rate_limit_max_requests, "redis.rate_limit_max_requests", err))
        return err;
    return "";
}

// 解析 "log" 段。
std::string ParseLog(const nlohmann::json& root, LogConfig& cfg) {
    if (!root.contains("log"))
        return "";
    const auto& l = root["log"];
    if (!l.is_object())
        return "log must be an object";
    if (l.contains("path"))
        return "log.path is no longer supported; use log.file_path";
    std::string err;
    if (!GetStr(l, "level", cfg.level, err))
        return "log." + err;
    if (!GetStr(l, "file_path", cfg.file_path, err))
        return "log." + err;
    if (!GetBool(l, "console", cfg.console, err))
        return "log." + err;
    if (!GetUInt(l, "max_size_mb", cfg.max_size_mb, "log.max_size_mb", err))
        return err;
    if (!GetUInt(l, "max_files", cfg.max_files, "log.max_files", err))
        return err;
    if (!GetBool(l, "async", cfg.async, err))
        return "log." + err;
    return "";
}

// 解析 "timeout" 段。
std::string ParseTimeout(const nlohmann::json& root, TimeoutConfig& cfg) {
    if (!root.contains("timeout"))
        return "";
    const auto& t = root["timeout"];
    if (!t.is_object())
        return "timeout must be an object";
    std::string err;
    if (!GetUInt(t, "remote_push_ms", cfg.remote_push_ms, "timeout.remote_push_ms", err))
        return err;
    return "";
}

}  // namespace

// ── ConfigLoader ─────────────────────────────────────────────────────────────

ConfigResult ConfigLoader::Load(const std::string& path) {
    std::string resolved = path;
    if (resolved.empty()) {
        if (const char* env_path = Env("CHAT_CONFIG_PATH")) {
            resolved = env_path;
        } else {
            resolved = "config/server.json";
        }
    }

    std::string err_msg;
    std::string content = ReadFile(resolved, err_msg);
    if (!err_msg.empty()) {
        return ConfigError{err_msg};
    }
    return ParseAndValidate(content);
}

ConfigResult ConfigLoader::LoadFromString(const std::string& json_str) { return ParseAndValidate(json_str); }

ConfigResult ConfigLoader::ParseAndValidate(const std::string& json_str) {
    // 1. 结构体默认值（通过成员初始化器已设置）
    ServerConfig config;

    // 2. 解析 JSON
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error& e) {
        return ConfigError{"JSON parse error: " + std::string(e.what())};
    }

    if (!root.is_object()) {
        return ConfigError{"config root must be a JSON object"};
    }

    std::string field_err;

    field_err = ParseServer(root, config.server);
    if (!field_err.empty())
        return ConfigError{field_err};

    field_err = ParseMysql(root, config.mysql, config.mysql_pool);
    if (!field_err.empty())
        return ConfigError{field_err};

    field_err = ParseRedis(root, config.redis);
    if (!field_err.empty())
        return ConfigError{field_err};

    field_err = ParseLog(root, config.log);
    if (!field_err.empty())
        return ConfigError{field_err};

    field_err = ParseTimeout(root, config.timeout);
    if (!field_err.empty())
        return ConfigError{field_err};

    // 3. 环境变量覆盖（非法值立即报错，不静默跳过）
    std::string env_err = ApplyEnvOverrides(config);
    if (!env_err.empty())
        return ConfigError{env_err};

    // 4. 全量校验
    std::string validation_err = Validate(config);
    if (!validation_err.empty())
        return ConfigError{validation_err};

    return config;
}

// 返回首个环境变量解析错误；全部成功时返回空字符串。
// 非法格式（如 "abc"）或超出范围（如 "65537"）均视为错误，而不是静默跳过。
std::string ConfigLoader::ApplyEnvOverrides(ServerConfig& config) {
    // MySQL 覆盖
    if (const char* v = Env("CHAT_DB_HOST"))
        config.mysql.host = v;
    if (const char* v = Env("CHAT_DB_USER"))
        config.mysql.username = v;
    if (const char* v = Env("CHAT_DB_PASSWORD"))
        config.mysql.password = v;
    if (const char* v = Env("CHAT_DB_NAME"))
        config.mysql.database = v;
    if (const char* v = Env("CHAT_DB_PORT")) {
        uint32_t raw = 0;
        if (!StrictParseInt(v, raw) || raw < 1 || raw > 65535) {
            return std::string("CHAT_DB_PORT is not a valid port number: ") + v;
        }
        config.mysql.port = static_cast<uint16_t>(raw);
    }

    // Redis 覆盖
    if (const char* v = Env("CHAT_REDIS_HOST"))
        config.redis.host = v;
    if (const char* v = Env("CHAT_REDIS_PASSWORD"))
        config.redis.password = v;
    if (const char* v = Env("CHAT_REDIS_PORT")) {
        uint32_t raw = 0;
        if (!StrictParseInt(v, raw) || raw < 1 || raw > 65535) {
            return std::string("CHAT_REDIS_PORT is not a valid port number: ") + v;
        }
        config.redis.port = static_cast<uint16_t>(raw);
    }
    if (const char* v = Env("CHAT_REDIS_DB")) {
        int db = 0;
        if (!StrictParseInt(v, db)) {
            return std::string("CHAT_REDIS_DB is not a valid integer: ") + v;
        }
        config.redis.database = db;
    }
    return "";
}

std::string ConfigLoader::Validate(const ServerConfig& config) {
    // server 段
    {
        struct in_addr addr4;
        struct in6_addr addr6;
        if (inet_pton(AF_INET, config.server.listen_ip.c_str(), &addr4) != 1 &&
            inet_pton(AF_INET6, config.server.listen_ip.c_str(), &addr6) != 1) {
            return "server.listen_ip is not a valid IP address: " + config.server.listen_ip;
        }
        if (config.server.listen_port < 1) {
            return "server.listen_port must be in range 1..65535";
        }
    }

    // mysql 段
    {
        if (config.mysql.host.empty())
            return "mysql.host must not be empty";
        if (config.mysql.username.empty())
            return "mysql.username must not be empty";
        if (config.mysql.database.empty())
            return "mysql.database must not be empty";
        if (config.mysql.port < 1)
            return "mysql.port must be in range 1..65535";
        if (config.mysql.connect_timeout_seconds == 0)
            return "mysql.connect_timeout_seconds must be positive";
        if (config.mysql.read_timeout_seconds == 0)
            return "mysql.read_timeout_seconds must be positive";
        if (config.mysql.write_timeout_seconds == 0)
            return "mysql.write_timeout_seconds must be positive";
    }

    // mysql pool 段
    {
        if (config.mysql_pool.max_connections == 0)
            return "mysql.pool.max_connections must be positive";
        if (config.mysql_pool.min_connections > config.mysql_pool.max_connections)
            return "mysql.pool.min_connections must be <= mysql.pool.max_connections";
        if (config.mysql_pool.borrow_timeout_ms == 0)
            return "mysql.pool.borrow_timeout_ms must be positive";
    }

    // redis 段（仅在 enabled 时强制校验连接参数）
    if (config.redis.enabled) {
        if (config.redis.host.empty())
            return "redis.host must not be empty";
        if (config.redis.port < 1)
            return "redis.port must be in range 1..65535";
        if (config.redis.pool_size <= 0)
            return "redis.pool_size must be positive";
        if (config.redis.database < 0)
            return "redis.database must be >= 0";
        if (config.redis.connect_timeout_ms <= 0)
            return "redis.connect_timeout_ms must be positive";
        if (config.redis.session_ttl_seconds <= 0)
            return "redis.session_ttl_seconds must be positive";
        if (config.redis.rate_limit_window_seconds <= 0)
            return "redis.rate_limit_window_seconds must be positive";
        if (config.redis.rate_limit_max_requests <= 0)
            return "redis.rate_limit_max_requests must be positive";
    }

    // log 段
    {
        if (config.log.level != "debug" && config.log.level != "info" && config.log.level != "warn" &&
            config.log.level != "error") {
            return "log.level must be one of: debug, info, warn, error";
        }
        if (config.log.max_size_mb == 0)
            return "log.max_size_mb must be positive";
        if (config.log.max_files == 0)
            return "log.max_files must be at least 1";
        constexpr std::size_t kBytesPerMegabyte = 1024 * 1024;
        if (config.log.max_size_mb > std::numeric_limits<std::size_t>::max() / kBytesPerMegabyte)
            return "log.max_size_mb is too large";
        if (!config.log.console && config.log.file_path.empty())
            return "log must enable console or provide file_path";
    }

    // timeout 段
    if (config.timeout.remote_push_ms == 0)
        return "timeout.remote_push_ms must be positive";

    return "";
}

// ── ParseConfigPathArg ────────────────────────────────────────────────────────

std::string ParseConfigPathArg(int argc, char* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--config") {
            return argv[i + 1];
        }
    }
    return "";
}

// ── ServerConfig::ToSafeString ────────────────────────────────────────────────

std::string ServerConfig::ToSafeString() const {
    // 从类型化结构重新构造安全 JSON，不修改原始对象。
    nlohmann::json j;

    j["server"]["listen_ip"] = server.listen_ip;
    j["server"]["listen_port"] = server.listen_port;

    j["mysql"]["host"] = mysql.host;
    j["mysql"]["port"] = mysql.port;
    j["mysql"]["username"] = mysql.username;
    j["mysql"]["password"] = "<redacted>";  // 脱敏
    j["mysql"]["database"] = mysql.database;
    j["mysql"]["connect_timeout_seconds"] = mysql.connect_timeout_seconds;
    j["mysql"]["read_timeout_seconds"] = mysql.read_timeout_seconds;
    j["mysql"]["write_timeout_seconds"] = mysql.write_timeout_seconds;
    j["mysql"]["pool"]["min_connections"] = mysql_pool.min_connections;
    j["mysql"]["pool"]["max_connections"] = mysql_pool.max_connections;
    j["mysql"]["pool"]["borrow_timeout_ms"] = mysql_pool.borrow_timeout_ms;

    j["redis"]["enabled"] = redis.enabled;
    j["redis"]["host"] = redis.host;
    j["redis"]["port"] = redis.port;
    j["redis"]["password"] = "<redacted>";  // 脱敏
    j["redis"]["database"] = redis.database;
    j["redis"]["pool_size"] = redis.pool_size;
    j["redis"]["key_prefix"] = redis.key_prefix;
    j["redis"]["session_ttl_seconds"] = redis.session_ttl_seconds;
    j["redis"]["rate_limit_window_seconds"] = redis.rate_limit_window_seconds;
    j["redis"]["rate_limit_max_requests"] = redis.rate_limit_max_requests;

    j["log"]["level"] = log.level;
    j["log"]["file_path"] = log.file_path;
    j["log"]["console"] = log.console;
    j["log"]["max_size_mb"] = log.max_size_mb;
    j["log"]["max_files"] = log.max_files;
    j["log"]["async"] = log.async;

    j["timeout"]["remote_push_ms"] = timeout.remote_push_ms;

    return j.dump(2);
}

}  // namespace chat
