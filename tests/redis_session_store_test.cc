#include "server/redis_session_store.h"

#include <cassert>
#include <iostream>

#include "fake_redis_client.h"

namespace {

chat::ConnectionSession MakeSession(chat::UserId user_id, const std::string &token) {
    return {.authenticated = true, .user_id = user_id, .username = "alice", .token = token};
}

void TestBindReadRefreshAndClear() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::RedisSessionStore store(&redis, config);
    const chat::ConnectionSession session = MakeSession(10001, "token-a");

    assert(store.Bind(42, session, 123456));
    assert(redis.ttls[store.TokenKey("token-a")] == 604800);
    assert(redis.ttls[store.UserPresenceKey(10001)] == 90);

    const auto token = store.GetToken("token-a");
    assert(token && token->user_id == 10001 && token->username == "alice" && token->issued_at == 123456);
    const auto presence = store.GetPresence(10001);
    assert(presence && presence->server_id == "server-a" && presence->connection_id == 42);

    redis.ttls[store.UserPresenceKey(10001)] = 1;
    assert(store.Refresh(42, session));
    assert(redis.ttls[store.UserPresenceKey(10001)] == 90);

    assert(store.ClearPresence(42, session));
    assert(!store.GetPresence(10001));
    assert(store.GetToken("token-a"));
    assert(store.RevokeToken("token-a"));
    assert(!store.GetToken("token-a"));
}

void TestRebindDoesNotLetOldConnectionDeleteNewPresence() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::RedisSessionStore store(&redis, config);
    const chat::ConnectionSession old_session = MakeSession(10001, "old-token");
    const chat::ConnectionSession new_session = MakeSession(10001, "new-token");

    assert(store.Bind(42, old_session, 1));
    assert(store.Bind(77, new_session, 2));
    assert(store.ClearPresence(42, old_session));

    const auto presence = store.GetPresence(10001);
    assert(presence && presence->connection_id == 77 && presence->token == "new-token");
    assert(!store.Refresh(42, old_session));
    assert(store.Refresh(77, new_session));
}

void TestCommandFailureIsReported() {
    chat::test::FakeRedisClient redis;
    redis.fail_commands = true;
    chat::RedisSessionStore store(&redis, chat::RedisConfig{});
    assert(!store.Bind(42, MakeSession(10001, "token"), 1));
}

}  // namespace

int main() {
    TestBindReadRefreshAndClear();
    TestRebindDoesNotLetOldConnectionDeleteNewPresence();
    TestCommandFailureIsReported();
    std::cout << "[PASS] redis session store tests passed\n";
    return 0;
}
