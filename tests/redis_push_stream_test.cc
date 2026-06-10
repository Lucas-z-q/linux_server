#include "stream/redis_push_stream.h"

#include <algorithm>
#include <cassert>
#include <iostream>

#include "fake_redis_client.h"

namespace {

chat::RemotePushEvent MakeEvent() {
    return {.event_id = "message-1",
            .message_id = "message-1",
            .from_user_id = 1,
            .to_user_id = 2,
            .target_connection_id = 42,
            .payload = R"({"msg_type":"message_push"})"};
}

bool IsAcked(const chat::test::FakeRedisClient &redis, const std::string &stream, std::size_t index) {
    return redis.streams.at(stream).at(index).acked;
}

void TestPublishAndSuccessfulConsumption() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-b";
    chat::RedisPushStream stream(&redis, config);
    int delivered = 0;
    int marked = 0;
    stream.SetDeliveryCallback([&](const chat::RemotePushEvent &event) {
        assert(event.target_connection_id == 42);
        ++delivered;
        return chat::RemoteDeliveryOutcome::kDelivered;
    });
    stream.SetMarkDeliveredCallback([&](chat::UserId user_id, const std::string &message_id) {
        assert(user_id == 2);
        assert(message_id == "message-1");
        ++marked;
        return true;
    });

    assert(stream.Initialize());
    assert(stream.Publish("server-b", MakeEvent()));
    assert(stream.PollOnce());

    const std::string key = stream.PushStreamKey("server-b");
    assert(delivered == 1);
    assert(marked == 1);
    assert(IsAcked(redis, key, 0));
}

void TestDuplicateEventsDeliverBodyOnce() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-b";
    chat::RedisPushStream stream(&redis, config);
    int delivered = 0;
    stream.SetDeliveryCallback([&](const chat::RemotePushEvent &) {
        ++delivered;
        return chat::RemoteDeliveryOutcome::kDelivered;
    });
    stream.SetMarkDeliveredCallback([](chat::UserId, const std::string &) { return true; });

    assert(stream.Initialize());
    assert(stream.Publish("server-b", MakeEvent()));
    assert(stream.Publish("server-b", MakeEvent()));
    assert(stream.PollOnce());

    const std::string key = stream.PushStreamKey("server-b");
    assert(delivered == 1);
    assert(IsAcked(redis, key, 0));
    assert(IsAcked(redis, key, 1));
}

void TestPendingEntryIsRecovered() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-b";
    chat::RedisPushStream stream(&redis, config);
    int attempts = 0;
    stream.SetDeliveryCallback([&](const chat::RemotePushEvent &) {
        ++attempts;
        return attempts == 1 ? chat::RemoteDeliveryOutcome::kRetry : chat::RemoteDeliveryOutcome::kDelivered;
    });
    stream.SetMarkDeliveredCallback([](chat::UserId, const std::string &) { return true; });

    assert(stream.Initialize());
    assert(stream.Publish("server-b", MakeEvent()));
    assert(stream.PollOnce());
    assert(!IsAcked(redis, stream.PushStreamKey("server-b"), 0));

    assert(stream.PollOnce());
    assert(attempts == 2);
    assert(IsAcked(redis, stream.PushStreamKey("server-b"), 0));
}

void TestInvalidAndMalformedTargetsAreAcked() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-b";
    chat::RedisPushStream stream(&redis, config);
    stream.SetDeliveryCallback(
        [](const chat::RemotePushEvent &) { return chat::RemoteDeliveryOutcome::kInvalidTarget; });
    stream.SetMarkDeliveredCallback([](chat::UserId, const std::string &) {
        assert(false);
        return false;
    });

    assert(stream.Initialize());
    assert(stream.Publish("server-b", MakeEvent()));
    auto &entries = redis.streams[stream.PushStreamKey("server-b")];
    entries.push_back({.id = "bad-0", .fields = {"message_id", "broken"}});
    assert(stream.PollOnce());

    assert(entries[0].acked);
    assert(entries[1].acked);
    assert(redis.streams[stream.DeadLetterStreamKey("server-b")].size() == 1);
}

void TestMarkFailureUsesBodyFreeRetryStream() {
    chat::test::FakeRedisClient redis;
    chat::RedisConfig config;
    config.server_id = "server-b";
    chat::RedisPushStream stream(&redis, config);
    stream.SetDeliveryCallback([](const chat::RemotePushEvent &) { return chat::RemoteDeliveryOutcome::kDelivered; });
    stream.SetMarkDeliveredCallback([](chat::UserId, const std::string &) { return false; });

    assert(stream.Initialize());
    assert(stream.Publish("server-b", MakeEvent()));
    assert(stream.PollOnce());

    const auto &retry = redis.streams[stream.RetryStreamKey("server-b")];
    assert(retry.size() == 1);
    assert(std::find(retry[0].fields.begin(), retry[0].fields.end(), "payload") == retry[0].fields.end());
    assert(IsAcked(redis, stream.PushStreamKey("server-b"), 0));
}

}  // namespace

int main() {
    TestPublishAndSuccessfulConsumption();
    TestDuplicateEventsDeliverBodyOnce();
    TestPendingEntryIsRecovered();
    TestInvalidAndMalformedTargetsAreAcked();
    TestMarkFailureUsesBodyFreeRetryStream();
    std::cout << "[PASS] redis push stream tests passed\n";
    return 0;
}
