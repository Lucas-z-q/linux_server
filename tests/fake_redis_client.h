#ifndef LINUX_SERVER_TESTS_FAKE_REDIS_CLIENT_H_
#define LINUX_SERVER_TESTS_FAKE_REDIS_CLIENT_H_

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "redis/redis_client.h"

namespace chat::test {

inline RedisCommandResult RedisOk(RedisReply reply = {}) {
    return {.success = true, .error = RedisError::kNone, .message = "ok", .reply = std::move(reply)};
}

inline RedisCommandResult RedisFailure(RedisError error, const std::string &message) {
    return {.success = false, .error = error, .message = message};
}

inline RedisReply StringReply(const std::string &value) {
    return {.type = RedisReplyType::kString, .string_value = value};
}

inline RedisReply IntegerReply(long long value) { return {.type = RedisReplyType::kInteger, .integer_value = value}; }

// 提供测试所需的最小 Redis 语义，并允许直接注入命令失败。
class FakeRedisClient : public IRedisClient {
   public:
    struct StreamEntry {
        std::string id;
        std::vector<std::string> fields;
        bool pending = false;
        bool acked = false;
    };

    RedisCommandResult Command(const std::vector<std::string> &args) override {
        std::lock_guard<std::mutex> lock(command_mutex_);
        commands.push_back(args);
        if (fail_commands) {
            return RedisFailure(RedisError::kCommandFailed, "injected failure");
        }
        if (args.empty()) {
            return RedisFailure(RedisError::kCommandFailed, "empty command");
        }
        if (args[0] == "DEL") {
            long long removed = 0;
            for (std::size_t i = 1; i < args.size(); ++i) {
                removed += hashes.erase(args[i]);
                removed += strings.erase(args[i]);
                counters.erase(args[i]);
                ttls.erase(args[i]);
            }
            return RedisOk(IntegerReply(removed));
        }
        if (args[0] == "GET") {
            const auto value = strings.find(args[1]);
            if (value == strings.end()) {
                return RedisOk();
            }
            return RedisOk(StringReply(value->second));
        }
        if (args[0] == "SETEX") {
            strings[args[1]] = args[3];
            ttls[args[1]] = std::stoll(args[2]);
            return RedisOk(StringReply("OK"));
        }
        if (args[0] == "SET") {
            if (args.size() >= 4 && args[3] == "NX" && strings.count(args[1]) != 0) {
                return RedisFailure(RedisError::kNotFound, "not set");
            }
            strings[args[1]] = args[2];
            if (args.size() >= 6 && args[4] == "EX") {
                ttls[args[1]] = std::stoll(args[5]);
            }
            return RedisOk(StringReply("OK"));
        }
        if (args[0] == "XGROUP") {
            return RedisOk(StringReply("OK"));
        }
        if (args[0] == "XADD") {
            return XAdd(args);
        }
        if (args[0] == "XREADGROUP") {
            return XReadGroup(args);
        }
        if (args[0] == "XAUTOCLAIM") {
            return XAutoClaim(args);
        }
        if (args[0] == "XACK") {
            return XAck(args);
        }
        if (args[0] == "HMGET") {
            RedisReply array;
            array.type = RedisReplyType::kArray;
            const auto hash = hashes.find(args[1]);
            for (std::size_t i = 2; i < args.size(); ++i) {
                const auto field = hash == hashes.end() ? std::unordered_map<std::string, std::string>::const_iterator{}
                                                        : hash->second.find(args[i]);
                if (hash == hashes.end() || field == hash->second.end()) {
                    array.elements.push_back({});
                } else {
                    array.elements.push_back(StringReply(field->second));
                }
            }
            return RedisOk(std::move(array));
        }
        if (args[0] == "EVAL") {
            if (args[1].find("INCR") != std::string::npos) {
                return EvalRateLimit(args);
            }
            if (args[1].find("old_server") != std::string::npos) {
                return EvalBind(args);
            }
            if (args[1].find("KEYS[3]") != std::string::npos) {
                return EvalRevoke(args);
            }
            if (args[1].find("EXPIRE") != std::string::npos) {
                return EvalRefresh(args);
            }
            return EvalClear(args);
        }
        return RedisFailure(RedisError::kCommandFailed, "unsupported fake command");
    }

    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> hashes;
    std::unordered_map<std::string, std::string> strings;
    std::unordered_map<std::string, long long> counters;
    std::unordered_map<std::string, long long> ttls;
    std::unordered_map<std::string, std::vector<StreamEntry>> streams;
    std::vector<std::vector<std::string>> commands;
    bool fail_commands = false;

   private:
    std::mutex command_mutex_;
    long long next_stream_id_ = 0;

    RedisCommandResult EvalRateLimit(const std::vector<std::string> &args) {
        const std::string &key = args[3];
        const long long count = ++counters[key];
        if (count == 1) {
            ttls[key] = std::stoll(args[4]);
        }
        RedisReply reply;
        reply.type = RedisReplyType::kArray;
        reply.elements.push_back(IntegerReply(count));
        reply.elements.push_back(IntegerReply(ttls[key]));
        return RedisOk(std::move(reply));
    }

    RedisReply BuildStreamEntries(const std::vector<StreamEntry *> &entries) {
        RedisReply array;
        array.type = RedisReplyType::kArray;
        for (const StreamEntry *entry : entries) {
            RedisReply item;
            item.type = RedisReplyType::kArray;
            item.elements.push_back(StringReply(entry->id));
            RedisReply fields;
            fields.type = RedisReplyType::kArray;
            for (const std::string &field : entry->fields) {
                fields.elements.push_back(StringReply(field));
            }
            item.elements.push_back(std::move(fields));
            array.elements.push_back(std::move(item));
        }
        return array;
    }

    RedisCommandResult XAdd(const std::vector<std::string> &args) {
        const std::string id = std::to_string(++next_stream_id_) + "-0";
        StreamEntry entry;
        entry.id = id;
        entry.fields.assign(args.begin() + 3, args.end());
        streams[args[1]].push_back(std::move(entry));
        return RedisOk(StringReply(id));
    }

    RedisCommandResult XReadGroup(const std::vector<std::string> &args) {
        auto streams_arg = std::find(args.begin(), args.end(), "STREAMS");
        if (streams_arg == args.end() || streams_arg + 1 == args.end()) {
            return RedisFailure(RedisError::kCommandFailed, "missing stream");
        }
        const std::string &stream_name = *(streams_arg + 1);
        std::vector<StreamEntry *> available;
        for (StreamEntry &entry : streams[stream_name]) {
            if (!entry.pending && !entry.acked) {
                entry.pending = true;
                available.push_back(&entry);
            }
        }
        if (available.empty()) {
            return RedisFailure(RedisError::kNotFound, "no entries");
        }
        RedisReply stream;
        stream.type = RedisReplyType::kArray;
        stream.elements.push_back(StringReply(stream_name));
        stream.elements.push_back(BuildStreamEntries(available));
        RedisReply root;
        root.type = RedisReplyType::kArray;
        root.elements.push_back(std::move(stream));
        return RedisOk(std::move(root));
    }

    RedisCommandResult XAutoClaim(const std::vector<std::string> &args) {
        std::vector<StreamEntry *> pending;
        for (StreamEntry &entry : streams[args[1]]) {
            if (entry.pending && !entry.acked) {
                pending.push_back(&entry);
            }
        }
        RedisReply root;
        root.type = RedisReplyType::kArray;
        root.elements.push_back(StringReply("0-0"));
        root.elements.push_back(BuildStreamEntries(pending));
        return RedisOk(std::move(root));
    }

    RedisCommandResult XAck(const std::vector<std::string> &args) {
        long long count = 0;
        for (StreamEntry &entry : streams[args[1]]) {
            if (entry.id == args[3] && !entry.acked) {
                entry.acked = true;
                ++count;
            }
        }
        return RedisOk(IntegerReply(count));
    }

    RedisCommandResult EvalBind(const std::vector<std::string> &args) {
        const std::string &token_key = args[3];
        const std::string &user_key = args[4];
        const std::string &conn_key = args[5];
        const std::string &user_session_key = args[6];
        const std::string &prefix = args[7];
        const std::string &server_id = args[8];
        const std::string &connection_id = args[9];
        const std::string &user_id = args[10];
        const std::string &username = args[11];
        const std::string &issued_at = args[12];
        const long long token_ttl = std::stoll(args[13]);
        const std::string &token = args[14];
        const long long presence_ttl = std::stoll(args[15]);

        const auto old_user_session = hashes.find(user_session_key);
        if (old_user_session != hashes.end()) {
            const auto old_token = old_user_session->second.find("token");
            if (old_token != old_user_session->second.end() && old_token->second != token) {
                const std::string old_token_key = prefix + ":session:token:" + old_token->second;
                hashes.erase(old_token_key);
                ttls.erase(old_token_key);
            }
        }

        const auto old_user_presence = hashes.find(user_key);
        if (old_user_presence != hashes.end()) {
            const auto old_server = old_user_presence->second.find("server_id");
            const auto old_conn = old_user_presence->second.find("connection_id");
            if (old_server != old_user_presence->second.end() && old_conn != old_user_presence->second.end()) {
                const std::string old_conn_key =
                    prefix + ":presence:conn:" + old_server->second + ":" + old_conn->second;
                hashes.erase(old_conn_key);
                ttls.erase(old_conn_key);
            }
        }

        const auto old_conn_presence = hashes.find(conn_key);
        if (old_conn_presence != hashes.end()) {
            const std::string old_user = old_conn_presence->second["user_id"];
            const std::string old_token = old_conn_presence->second["token"];
            const std::string old_user_key = prefix + ":presence:user:" + old_user;
            const auto old_presence = hashes.find(old_user_key);
            if (old_presence != hashes.end() && old_presence->second["server_id"] == server_id &&
                old_presence->second["connection_id"] == connection_id && old_presence->second["token"] == old_token) {
                hashes.erase(old_user_key);
                ttls.erase(old_user_key);
            }
        }

        hashes[token_key] = {{"user_id", user_id}, {"username", username}, {"issued_at", issued_at}};
        hashes[user_key] = {{"server_id", server_id}, {"connection_id", connection_id}, {"token", token}};
        hashes[conn_key] = {{"user_id", user_id}, {"token", token}};
        hashes[user_session_key] = {{"token", token}};
        ttls[token_key] = token_ttl;
        ttls[user_key] = presence_ttl;
        ttls[conn_key] = presence_ttl;
        ttls[user_session_key] = token_ttl;
        return RedisOk(IntegerReply(1));
    }

    RedisCommandResult EvalRefresh(const std::vector<std::string> &args) {
        const std::string &user_key = args[3];
        const std::string &conn_key = args[4];
        const auto user = hashes.find(user_key);
        const auto conn = hashes.find(conn_key);
        if (user == hashes.end() || conn == hashes.end() || user->second["server_id"] != args[5] ||
            user->second["connection_id"] != args[6] || user->second["token"] != args[7] ||
            conn->second["user_id"] != args[8] || conn->second["token"] != args[7]) {
            return RedisOk(IntegerReply(0));
        }
        ttls[user_key] = std::stoll(args[9]);
        ttls[conn_key] = std::stoll(args[9]);
        return RedisOk(IntegerReply(1));
    }

    RedisCommandResult EvalRevoke(const std::vector<std::string> &args) {
        const std::string &user_key = args[3];
        const std::string &conn_key = args[4];
        const std::string &token_key = args[5];
        const std::string &user_session_key = args[6];
        hashes.erase(token_key);
        ttls.erase(token_key);
        const auto current = hashes.find(user_session_key);
        if (current != hashes.end() && current->second["token"] == args[9]) {
            hashes.erase(user_session_key);
            ttls.erase(user_session_key);
        }
        hashes.erase(conn_key);
        ttls.erase(conn_key);
        const auto user = hashes.find(user_key);
        if (user != hashes.end() && user->second["server_id"] == args[7] && user->second["connection_id"] == args[8] &&
            user->second["token"] == args[9]) {
            hashes.erase(user_key);
            ttls.erase(user_key);
        }
        return RedisOk(IntegerReply(1));
    }

    RedisCommandResult EvalClear(const std::vector<std::string> &args) {
        const std::string &user_key = args[3];
        const std::string &conn_key = args[4];
        hashes.erase(conn_key);
        ttls.erase(conn_key);
        const auto user = hashes.find(user_key);
        if (user != hashes.end() && user->second["server_id"] == args[5] && user->second["connection_id"] == args[6] &&
            user->second["token"] == args[7]) {
            hashes.erase(user_key);
            ttls.erase(user_key);
        }
        return RedisOk(IntegerReply(1));
    }
};

class FakeRedisConnection : public RedisConnection {
   public:
    FakeRedisConnection(const RedisConfig &config, std::size_t id, RedisCommandResult connect_result)
        : RedisConnection(config), id_(id), connect_result_(std::move(connect_result)) {}

    RedisConnectionResult Connect() override {
        connected_ = connect_result_.ok();
        return connect_result_;
    }
    RedisCommandResult Execute(const std::vector<std::string> &args) override {
        (void)args;
        return connected_ ? RedisOk() : RedisFailure(RedisError::kConnectionUnavailable, "not connected");
    }
    void Close() noexcept override { connected_ = false; }
    bool IsConnected() const noexcept override { return connected_; }
    std::size_t id() const noexcept { return id_; }

   private:
    std::size_t id_;
    RedisCommandResult connect_result_;
    bool connected_ = false;
};

class FakeRedisConnectionFactory : public IRedisConnectionFactory {
   public:
    std::unique_ptr<RedisConnection> Create(const RedisConfig &config) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::make_unique<FakeRedisConnection>(config, ++created_count_, connect_result_);
    }

    void SetConnectResult(RedisCommandResult result) {
        std::lock_guard<std::mutex> lock(mutex_);
        connect_result_ = std::move(result);
    }
    std::size_t created_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return created_count_;
    }

   private:
    mutable std::mutex mutex_;
    RedisCommandResult connect_result_ = RedisOk();
    std::size_t created_count_ = 0;
};

}  // namespace chat::test

#endif  // LINUX_SERVER_TESTS_FAKE_REDIS_CLIENT_H_
