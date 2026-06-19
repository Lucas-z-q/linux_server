#include <unistd.h>

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "redis/redis_client.h"
#include "redis/redis_pool.h"
#include "server/redis_session_store.h"

namespace {

chat::RedisConfig LoadConfig() {
    chat::RedisConfig config;
    config.enabled = true;
    if (const char* host = std::getenv("CHAT_REDIS_TEST_HOST")) {
        config.host = host;
    }
    if (const char* port = std::getenv("CHAT_REDIS_TEST_PORT")) {
        config.port = static_cast<std::uint16_t>(std::stoi(port));
    }
    if (const char* password = std::getenv("CHAT_REDIS_TEST_PASSWORD")) {
        config.password = password;
    }
    config.database = 15;
    if (const char* database = std::getenv("CHAT_REDIS_TEST_DB")) {
        config.database = std::stoi(database);
    }
    config.pool_size = 2;
    config.connect_timeout_ms = 1000;
    config.command_timeout_ms = 1000;
    config.server_id = "security-integration";
    config.key_prefix = "chat:security:session:" + std::to_string(getpid());
    config.session_ttl_seconds = 1;
    config.presence_ttl_seconds = 5;
    return config;
}

chat::ConnectionSession Session(chat::UserId user_id, const std::string& token) {
    return {.authenticated = true, .user_id = user_id, .username = "alice", .token = token};
}

void Cleanup(chat::IRedisClient* client, const chat::RedisSessionStore& store,
             const std::vector<std::pair<chat::ConnectionId, std::string>>& sessions) {
    std::vector<std::string> command = {"DEL", store.UserPresenceKey(42), store.UserSessionKey(42)};
    for (const auto& session : sessions) {
        command.push_back(store.TokenKey(session.second));
        command.push_back(store.ConnectionPresenceKey("security-integration", session.first));
    }
    client->Command(command);
}

bool TestRealRedisSessionSecurity() {
    const chat::RedisConfig config = LoadConfig();
    chat::RedisPool pool(config);
    const chat::RedisPoolInitResult initialized = pool.Init();
    if (!initialized.ok()) {
        std::cout << "[SKIP] Redis integration unavailable\n";
        return false;
    }
    chat::RedisClient client(&pool);
    chat::RedisSessionStore store(&client, config);
    const std::string old_token(64, 'a');
    const std::string new_token(64, 'b');
    const std::string expiring_token(64, 'c');
    const std::string corrupt_token(64, 'd');
    const std::vector<std::pair<chat::ConnectionId, std::string>> sessions = {
        {11, old_token},
        {22, new_token},
        {33, expiring_token},
        {44, corrupt_token},
    };
    Cleanup(&client, store, sessions);

    const chat::ConnectionSession old_session = Session(42, old_token);
    const chat::ConnectionSession new_session = Session(42, new_token);
    assert(store.Bind(11, old_session, 100));
    assert(store.GetToken(old_token).has_value());
    assert(store.Bind(22, new_session, 101));
    assert(!store.GetToken(old_token).has_value());
    assert(store.GetToken(new_token).has_value());

    assert(store.ClearPresence(11, old_session));
    const auto current_presence = store.GetPresence(42);
    assert(current_presence.has_value());
    assert(current_presence->connection_id == 22);
    assert(current_presence->token == new_token);

    assert(store.RevokeSession(22, new_session));
    assert(!store.GetToken(new_token).has_value());
    assert(!store.GetPresence(42).has_value());

    const chat::ConnectionSession expiring = Session(42, expiring_token);
    assert(store.Bind(33, expiring, 102));
    assert(store.GetToken(expiring_token).has_value());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    assert(!store.GetToken(expiring_token).has_value());

    const chat::RedisCommandResult corrupt = client.Command(
        {"HSET", store.TokenKey(corrupt_token), "user_id", "broken", "username", "alice", "issued_at", "1"});
    assert(corrupt.ok());
    assert(!store.GetToken(corrupt_token).has_value());
    const chat::RedisCommandResult exists = client.Command({"EXISTS", store.TokenKey(corrupt_token)});
    assert(exists.ok());
    assert(exists.reply.type == chat::RedisReplyType::kInteger);
    assert(exists.reply.integer_value == 0);

    Cleanup(&client, store, sessions);
    std::cout << "[PASS] redis session security integration tests passed\n";
    return true;
}

}  // namespace

int main() { return TestRealRedisSessionSecurity() ? 0 : 77; }
