#include "config/redis_config.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class EnvGuard {
   public:
    explicit EnvGuard(std::vector<std::string> names) : names_(std::move(names)) {
        for (const std::string &name : names_) {
            const char *value = std::getenv(name.c_str());
            old_[name] = value == nullptr ? std::nullopt : std::optional<std::string>(value);
            unsetenv(name.c_str());
        }
    }
    ~EnvGuard() {
        for (const std::string &name : names_) {
            if (old_[name]) {
                setenv(name.c_str(), old_[name]->c_str(), 1);
            } else {
                unsetenv(name.c_str());
            }
        }
    }
    void Set(const std::string &name, const std::string &value) { setenv(name.c_str(), value.c_str(), 1); }

   private:
    std::vector<std::string> names_;
    std::unordered_map<std::string, std::optional<std::string>> old_;
};

std::vector<std::string> Names() {
    return {"CHAT_REDIS_ENABLED",
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
            "CHAT_PRESENCE_TTL_SECONDS"};
}

void TestDefaults() {
    EnvGuard env(Names());
    const auto result = chat::LoadRedisConfigFromEnv();
    assert(result.ok());
    assert(!result.config.enabled);
    assert(result.config.host == "127.0.0.1");
    assert(result.config.port == 6379);
    assert(result.config.pool_size == 4);
    assert(result.config.connect_timeout_ms == 500);
    assert(result.config.command_timeout_ms == 500);
    assert(result.config.key_prefix == "chat");
    assert(result.config.server_id == "server-1");
    assert(result.config.session_ttl_seconds == 604800);
    assert(result.config.presence_ttl_seconds == 90);
}

void TestOverrides() {
    EnvGuard env(Names());
    env.Set("CHAT_REDIS_ENABLED", "1");
    env.Set("CHAT_REDIS_HOST", "10.0.0.8");
    env.Set("CHAT_REDIS_PORT", "6380");
    env.Set("CHAT_REDIS_DB", "3");
    env.Set("CHAT_REDIS_POOL_SIZE", "8");
    env.Set("CHAT_REDIS_CONNECT_TIMEOUT_MS", "100");
    env.Set("CHAT_REDIS_COMMAND_TIMEOUT_MS", "200");
    env.Set("CHAT_REDIS_KEY_PREFIX", "test");
    env.Set("CHAT_SERVER_ID", "server-a");
    env.Set("CHAT_SESSION_TTL_SECONDS", "60");
    env.Set("CHAT_PRESENCE_TTL_SECONDS", "30");
    const auto result = chat::LoadRedisConfigFromEnv();
    assert(result.ok());
    assert(result.config.enabled);
    assert(result.config.host == "10.0.0.8");
    assert(result.config.port == 6380);
    assert(result.config.database == 3);
    assert(result.config.pool_size == 8);
    assert(result.config.connect_timeout_ms == 100);
    assert(result.config.command_timeout_ms == 200);
    assert(result.config.key_prefix == "test");
    assert(result.config.server_id == "server-a");
}

void TestInvalidValues() {
    {
        EnvGuard env(Names());
        env.Set("CHAT_REDIS_ENABLED", "yes");
        assert(!chat::LoadRedisConfigFromEnv().ok());
    }
    {
        EnvGuard env(Names());
        env.Set("CHAT_REDIS_PORT", "70000");
        assert(!chat::LoadRedisConfigFromEnv().ok());
    }
    {
        EnvGuard env(Names());
        env.Set("CHAT_REDIS_POOL_SIZE", "0");
        assert(!chat::LoadRedisConfigFromEnv().ok());
    }
    {
        EnvGuard env(Names());
        env.Set("CHAT_SERVER_ID", "bad:id");
        assert(!chat::LoadRedisConfigFromEnv().ok());
    }
}

}  // namespace

int main() {
    TestDefaults();
    TestOverrides();
    TestInvalidValues();
    std::cout << "[PASS] redis config tests passed\n";
    return 0;
}
