#ifndef LINUX_SERVER_INCLUDE_STREAM_REDIS_PUSH_STREAM_H_
#define LINUX_SERVER_INCLUDE_STREAM_REDIS_PUSH_STREAM_H_

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "config/redis_config.h"
#include "redis/redis_client.h"
#include "stream/remote_push.h"

namespace chat {

class RedisPushStream : public IRemotePushPublisher {
   public:
    using DeliveryCallback = std::function<RemoteDeliveryOutcome(const RemotePushEvent &)>;
    using MarkDeliveredCallback = std::function<bool(UserId, const std::string &)>;

    RedisPushStream(IRedisClient *redis, RedisConfig config);
    ~RedisPushStream();

    RedisPushStream(const RedisPushStream &) = delete;
    RedisPushStream &operator=(const RedisPushStream &) = delete;

    bool Initialize();
    bool Publish(const std::string &target_server_id, const RemotePushEvent &event) override;

    void SetDeliveryCallback(DeliveryCallback callback);
    void SetMarkDeliveredCallback(MarkDeliveredCallback callback);
    void Start();
    void Stop();

    // 单次轮询便于 deterministic 单元测试，生产环境由后台线程重复调用。
    bool PollOnce();

    std::string PushStreamKey(const std::string &server_id) const;
    std::string RetryStreamKey(const std::string &server_id) const;
    std::string DeadLetterStreamKey(const std::string &server_id) const;

   private:
    struct StreamEntry {
        std::string stream_id;
        std::unordered_map<std::string, std::string> fields;
    };

    struct ReadBatch {
        bool success = false;
        std::vector<StreamEntry> entries;
    };

    bool EnsureGroup(const std::string &stream, const std::string &group);
    ReadBatch ReadNew(const std::string &stream, const std::string &group, bool block);
    ReadBatch ClaimPending(const std::string &stream, const std::string &group);
    std::vector<StreamEntry> ParseReadReply(const RedisReply &reply) const;
    std::vector<StreamEntry> ParseClaimReply(const RedisReply &reply) const;
    bool ProcessPushEntry(const StreamEntry &entry);
    bool ProcessRetryEntry(const StreamEntry &entry);
    bool Ack(const std::string &stream, const std::string &group, const std::string &stream_id);
    bool MoveToDeadLetter(const std::string &source_stream, const StreamEntry &entry, const std::string &reason);
    bool PublishDeliveryRetry(UserId user_id, const std::string &message_id);
    std::optional<RemotePushEvent> DecodePushEvent(const StreamEntry &entry) const;
    std::string DeliveryDedupKey(const std::string &message_id) const;
    void Run();

    IRedisClient *redis_;
    RedisConfig config_;
    DeliveryCallback delivery_callback_;
    MarkDeliveredCallback mark_delivered_callback_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

}  // namespace chat

#endif  // LINUX_SERVER_INCLUDE_STREAM_REDIS_PUSH_STREAM_H_
