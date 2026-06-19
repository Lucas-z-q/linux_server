#include "server/redis_session_store.h"

#include <cassert>
#include <iostream>

#include "fake_redis_client.h"

namespace {

std::string Token(char value) { return std::string(64, value); }

chat::ConnectionSession MakeSession(chat::UserId user_id, const std::string &token) {
    return {.authenticated = true, .user_id = user_id, .username = "alice", .token = token};
}

void TestBindReadRefreshAndClear() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::RedisSessionStore store(&redis, config);
    const std::string token_value = Token('a');
    const chat::ConnectionSession session = MakeSession(10001, token_value);

    assert(store.Bind(42, session, 123456));
    assert(redis.ttls[store.TokenKey(token_value)] == 604800);
    assert(redis.ttls[store.UserSessionKey(10001)] == 604800);
    assert(redis.ttls[store.UserPresenceKey(10001)] == 120);

    const auto token = store.GetToken(token_value);
    assert(token && token->user_id == 10001 && token->username == "alice" && token->issued_at == 123456);
    const auto presence = store.GetPresence(10001);
    assert(presence && presence->server_id == "server-a" && presence->connection_id == 42);

    redis.ttls[store.UserPresenceKey(10001)] = 1;
    assert(store.Refresh(42, session));
    assert(redis.ttls[store.UserPresenceKey(10001)] == 120);

    assert(store.ClearPresence(42, session));
    assert(!store.GetPresence(10001));
    assert(store.GetToken(token_value));
    assert(store.RevokeSession(42, session));
    assert(!store.GetToken(token_value));
}

void TestRebindDoesNotLetOldConnectionDeleteNewPresence() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-a";
    chat::RedisSessionStore store(&redis, config);
    const std::string old_token = Token('a');
    const std::string new_token = Token('b');
    const chat::ConnectionSession old_session = MakeSession(10001, old_token);
    const chat::ConnectionSession new_session = MakeSession(10001, new_token);

    assert(store.Bind(42, old_session, 1));
    redis.hashes.erase(store.UserPresenceKey(10001));
    redis.ttls.erase(store.UserPresenceKey(10001));
    assert(store.Bind(77, new_session, 2));
    assert(!store.GetToken(old_token));
    assert(store.GetToken(new_token));
    assert(store.ClearPresence(42, old_session));

    const auto presence = store.GetPresence(10001);
    assert(presence && presence->connection_id == 77 && presence->token == new_token);
    assert(!store.Refresh(42, old_session));
    assert(store.Refresh(77, new_session));
}

void TestMalformedTokenDataFailsClosed() {
    chat::test::FakeRedisClient redis;
    chat::RedisSessionStore store(&redis, chat::RedisConfig{});
    const std::string broken_token = Token('c');
    redis.hashes[store.TokenKey(broken_token)] = {
        {"user_id", "not-a-number"}, {"username", "alice"}, {"issued_at", "1"}};
    assert(!store.GetToken(broken_token));
    assert(redis.hashes.count(store.TokenKey(broken_token)) == 0);
    assert(!store.GetToken("malformed-token"));
}

void TestCommandFailureIsReported() {
    chat::test::FakeRedisClient redis;
    redis.fail_commands = true;
    chat::RedisSessionStore store(&redis, chat::RedisConfig{});
    const std::string token = Token('d');
    assert(!store.Bind(42, MakeSession(10001, token), 1));
    assert(!store.GetToken(token));
    assert(!store.RevokeSession(42, MakeSession(10001, token)));
    assert(!store.Bind(42, MakeSession(10001, "malformed-token"), 1));
}

}  // namespace

int main() {
    TestBindReadRefreshAndClear();
    TestRebindDoesNotLetOldConnectionDeleteNewPresence();
    TestMalformedTokenDataFailsClosed();
    TestCommandFailureIsReported();
    std::cout << "[PASS] redis session store tests passed\n";
    return 0;
}
