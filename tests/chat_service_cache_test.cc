#include "cache/cached_user_repository.h"
#include "service/chat_service.h"

#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>

#include "fake_message_repository.h"
#include "fake_redis_client.h"
#include "fake_user_repository.h"

namespace {

// 与 chat_service_redis_test 一致的最小 ISessionManager：只记录绑定与清理。
class FakeSessionManager : public chat::ISessionManager {
   public:
    std::unordered_map<chat::ConnectionId, chat::ConnectionSession> sessions;

    bool BindSession(chat::ConnectionId connection_id, const chat::ConnectionSession &session) override {
        sessions[connection_id] = session;
        return true;
    }
    std::optional<chat::ConnectionId> GetConnectionId(chat::UserId user_id) override {
        (void)user_id;
        return std::nullopt;
    }
    std::optional<chat::ConnectionSession> GetSession(chat::ConnectionId connection_id) override {
        const auto it = sessions.find(connection_id);
        return it == sessions.end() ? std::nullopt : std::optional<chat::ConnectionSession>(it->second);
    }
    void ClearSession(chat::ConnectionId connection_id) override { sessions.erase(connection_id); }
};

void BindSender(FakeSessionManager *sessions) {
    chat::ConnectionSession session;
    session.authenticated = true;
    session.user_id = 1;
    session.username = "user1";
    sessions->BindSession(100, session);
}

chat::SendMessageRequest MakeRequest(chat::UserId to_user_id, const std::string &client_msg_id) {
    chat::SendMessageRequest request;
    request.client_msg_id = client_msg_id;
    request.to_user_id = to_user_id;
    request.content = "hello";
    return request;
}

// ponytail: CachedUserRepository 只缓存 not-found（防穿透），不缓存正向结果；
// 这一点由 cached_user_repository_test 的 find_id_calls==2 断言固定。因此这里
// 验证 ChatService 接入缓存后的真实行为，而非 test.md 设想的“正向缓存命中”。

// 缓存 miss：发送给存在用户，回源一次，发送成功。
void TestExistingUserSendSucceedsOnCacheMiss() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::test::FakeRedisClient redis;
    chat::CachedUserRepository cached(&users, &redis, {});
    chat::ChatService service(sessions, messages, cached);

    const auto result = service.sendMessage(100, MakeRequest(2, "c1"));

    assert(result.code == chat::ErrorCode::OK);
    assert(users.find_by_id_calls == 1);
    assert(messages.create_message_calls == 1);
}

// 空缓存命中：发送给不存在用户，第一次回源并写空缓存，第二次命中空缓存不再回源。
void TestNotFoundUserCachedAndSkipsSourceOnSecondLookup() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::test::FakeRedisClient redis;
    chat::CachedUserRepository cached(&users, &redis, {});
    chat::ChatService service(sessions, messages, cached);

    const auto first = service.sendMessage(100, MakeRequest(999999, "c2"));
    const auto second = service.sendMessage(100, MakeRequest(999999, "c3"));

    assert(first.code == chat::ErrorCode::USER_NOT_FOUND);
    assert(second.code == chat::ErrorCode::USER_NOT_FOUND);
    // 第一次回源并写入空缓存标记，第二次命中标记，不再访问源仓储。
    assert(users.find_by_id_calls == 1);
    assert(redis.strings.count("chat:user:id:999999") == 1);
}

// 缓存故障：Redis 不可用时发送给存在用户仍按 MySQL 路径成功。
void TestRedisDownStillSendsViaSource() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::test::FakeRedisClient redis;
    redis.fail_commands = true;
    chat::CachedUserRepository cached(&users, &redis, {});
    chat::ChatService service(sessions, messages, cached);

    const auto result = service.sendMessage(100, MakeRequest(2, "c4"));

    assert(result.code == chat::ErrorCode::OK);
    assert(users.find_by_id_calls == 1);
}

// 缓存故障 + 不存在用户：空缓存写不进去，每次都回源，仍返回 USER_NOT_FOUND。
void TestRedisDownNotFoundUserAlwaysHitsSource() {
    FakeSessionManager sessions;
    BindSender(&sessions);
    FakeMessageRepository messages;
    FakeUserRepository users;
    chat::test::FakeRedisClient redis;
    redis.fail_commands = true;
    chat::CachedUserRepository cached(&users, &redis, {});
    chat::ChatService service(sessions, messages, cached);

    const auto first = service.sendMessage(100, MakeRequest(999999, "c5"));
    const auto second = service.sendMessage(100, MakeRequest(999999, "c6"));

    assert(first.code == chat::ErrorCode::USER_NOT_FOUND);
    assert(second.code == chat::ErrorCode::USER_NOT_FOUND);
    // Redis 不可用，空缓存写不进去，两次都回源。
    assert(users.find_by_id_calls == 2);
}

}  // namespace

int main() {
    TestExistingUserSendSucceedsOnCacheMiss();
    TestNotFoundUserCachedAndSkipsSourceOnSecondLookup();
    TestRedisDownStillSendsViaSource();
    TestRedisDownNotFoundUserAlwaysHitsSource();
    std::cout << "[PASS] chat service cache tests passed\n";
    return 0;
}
