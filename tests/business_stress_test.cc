#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "codec/packet_codec.h"
#include "common/error_code.h"
#include "common/types.h"
#include "config/server_config.h"
#include "db/message_repository.h"
#include "db/user_repository.h"
#include "handler/message_handler.h"
#include "model/message_record.h"
#include "model/user_record.h"
#include "net/TcpServer.h"
#include "nlohmann/json.hpp"
#include "security/password_hasher.h"
#include "server/session_manager.h"
#include "service/chat_service.h"
#include "service/user_service.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr const char* kServerIp = "127.0.0.1";
constexpr const char* kPassword = "password123";
constexpr chat::UserId kFirstStressUserId = 100000;
constexpr int kMaxStoredErrors = 12;

enum class StressBackend {
    kMemory,
    kReal,
};

enum class PasswordHasherMode {
    kFast,
    kBcrypt,
};

struct StressUserPair {
    int pair_index = 0;
    std::string alice_username;
    std::string bob_username;
    chat::UserId alice_id = 0;
    chat::UserId bob_id = 0;
};

struct StressOptions {
    StressBackend backend = StressBackend::kMemory;
    PasswordHasherMode password_hasher = PasswordHasherMode::kFast;
    std::string config_path;
    std::string run_id;
    int pairs = 50;
    int messages_per_pair = 10;
    int warmup_messages = 0;
    int client_workers = 4;
    int port = 0;
    int connect_timeout_ms = 1000;
    int request_timeout_ms = 3000;
    int push_timeout_ms = 3000;
    int content_size = 32;
    bool prepare_users = true;
    bool verbose = false;
};

struct ParsedOptions {
    bool ok = false;
    bool help = false;
    StressOptions options;
    std::string error;
};

chat::UserId AliceUserId(int pair_index) { return kFirstStressUserId + static_cast<chat::UserId>(pair_index) * 2; }

chat::UserId BobUserId(int pair_index) { return AliceUserId(pair_index) + 1; }

std::string MakeContent(int pair_index, int message_index, int content_size) {
    std::string content = "stress_" + std::to_string(pair_index) + "_" + std::to_string(message_index) + "_";
    if (static_cast<int>(content.size()) < content_size) {
        content.append(static_cast<std::size_t>(content_size - content.size()), 'x');
    } else {
        content.resize(static_cast<std::size_t>(content_size));
    }
    return content;
}

std::string PairKey(chat::UserId user_a, chat::UserId user_b) {
    const chat::UserId min_id = std::min(user_a, user_b);
    const chat::UserId max_id = std::max(user_a, user_b);
    return std::to_string(min_id) + ":" + std::to_string(max_id);
}

std::string ConversationId(chat::UserId user_a, chat::UserId user_b) {
    const chat::UserId min_id = std::min(user_a, user_b);
    const chat::UserId max_id = std::max(user_a, user_b);
    return "conv_" + std::to_string(min_id) + "_" + std::to_string(max_id);
}

chat::Timestamp NowSeconds() { return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); }

std::string EncodeFakePasswordHash(const std::string& password) { return "fast$" + password; }

std::string BackendName(StressBackend backend) {
    switch (backend) {
        case StressBackend::kMemory:
            return "memory";
        case StressBackend::kReal:
            return "real";
    }
    return "memory";
}

std::string PasswordHasherName(PasswordHasherMode mode) {
    switch (mode) {
        case PasswordHasherMode::kFast:
            return "fast";
        case PasswordHasherMode::kBcrypt:
            return "bcrypt";
    }
    return "fast";
}

bool ParseBackend(const std::string& value, StressBackend* out) {
    if (value == "memory") {
        *out = StressBackend::kMemory;
        return true;
    }
    if (value == "real") {
        *out = StressBackend::kReal;
        return true;
    }
    return false;
}

bool ParsePasswordHasherMode(const std::string& value, PasswordHasherMode* out) {
    if (value == "fast") {
        *out = PasswordHasherMode::kFast;
        return true;
    }
    if (value == "bcrypt") {
        *out = PasswordHasherMode::kBcrypt;
        return true;
    }
    return false;
}

bool IsValidRunId(const std::string& run_id) {
    if (run_id.empty() || run_id.size() > 64) {
        return false;
    }
    for (char ch : run_id) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

std::string GenerateRunId() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm local_time;
    localtime_r(&now_time, &local_time);

    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::ostringstream out;
    out << std::put_time(&local_time, "%Y%m%d_%H%M%S") << "_" << (micros % 1000000);
    return out.str();
}

std::string StressUsername(const std::string& run_id, const std::string& role, int pair_index) {
    const std::string run_suffix = run_id.size() > 16 ? run_id.substr(run_id.size() - 16) : run_id;
    const char role_code = role.empty() ? 'u' : role[0];
    std::ostringstream out;
    out << "st_" << run_suffix << "_" << role_code << "_" << std::setw(6) << std::setfill('0') << pair_index;
    return out.str();
}

std::vector<StressUserPair> BuildMemoryUserPairs(const std::string& run_id, int pairs) {
    std::vector<StressUserPair> result;
    result.reserve(static_cast<std::size_t>(pairs));
    for (int i = 0; i < pairs; ++i) {
        result.push_back(StressUserPair{i, StressUsername(run_id, "alice", i), StressUsername(run_id, "bob", i),
                                        AliceUserId(i), BobUserId(i)});
    }
    return result;
}

std::string MakeClientMessageId(const std::string& run_id, const std::string& phase, int pair_index,
                                int message_index) {
    return run_id + "_" + phase + "_" + std::to_string(pair_index) + "_" + std::to_string(message_index);
}

void PrintUsage(std::ostream& output) {
    output << "Usage: business_stress_test [options]\n"
           << "  --backend=memory|real\n"
           << "  --config=PATH\n"
           << "  --password-hasher=fast|bcrypt\n"
           << "  --run-id=VALUE\n"
           << "  --prepare-users=true|false\n"
           << "  --warmup-messages=N\n"
           << "  --pairs=N\n"
           << "  --messages-per-pair=N\n"
           << "  --client-workers=N\n"
           << "  --port=N\n"
           << "  --connect-timeout-ms=N\n"
           << "  --request-timeout-ms=N\n"
           << "  --push-timeout-ms=N\n"
           << "  --content-size=N\n"
           << "  --verbose=true|false\n";
}

bool ParseInteger(const std::string& value, int min_value, int max_value, int* out) {
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return false;
    }
    if (parsed < min_value || parsed > max_value) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool ParseBool(const std::string& value, bool* out) {
    if (value == "true" || value == "1") {
        *out = true;
        return true;
    }
    if (value == "false" || value == "0") {
        *out = false;
        return true;
    }
    return false;
}

ParsedOptions ParseOptions(int argc, char** argv) {
    ParsedOptions parsed;
    parsed.options = StressOptions{};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            parsed.help = true;
            parsed.ok = true;
            return parsed;
        }
        if (arg == "--verbose") {
            parsed.options.verbose = true;
            continue;
        }
        const std::size_t eq = arg.find('=');
        if (eq == std::string::npos || arg.compare(0, 2, "--") != 0) {
            parsed.error = "invalid option format: " + arg;
            return parsed;
        }
        const std::string name = arg.substr(2, eq - 2);
        const std::string value = arg.substr(eq + 1);

        bool valid = false;
        if (name == "backend") {
            valid = ParseBackend(value, &parsed.options.backend);
        } else if (name == "config") {
            parsed.options.config_path = value;
            valid = !value.empty();
        } else if (name == "password-hasher") {
            valid = ParsePasswordHasherMode(value, &parsed.options.password_hasher);
        } else if (name == "run-id") {
            parsed.options.run_id = value;
            valid = IsValidRunId(value);
        } else if (name == "prepare-users") {
            valid = ParseBool(value, &parsed.options.prepare_users);
        } else if (name == "warmup-messages") {
            valid = ParseInteger(value, 0, 100000, &parsed.options.warmup_messages);
        } else if (name == "pairs") {
            valid = ParseInteger(value, 1, 100000, &parsed.options.pairs);
        } else if (name == "messages-per-pair") {
            valid = ParseInteger(value, 1, 100000, &parsed.options.messages_per_pair);
        } else if (name == "client-workers") {
            valid = ParseInteger(value, 1, 1024, &parsed.options.client_workers);
        } else if (name == "port") {
            valid = ParseInteger(value, 0, 65535, &parsed.options.port);
        } else if (name == "connect-timeout-ms") {
            valid = ParseInteger(value, 1, 600000, &parsed.options.connect_timeout_ms);
        } else if (name == "request-timeout-ms") {
            valid = ParseInteger(value, 1, 600000, &parsed.options.request_timeout_ms);
        } else if (name == "push-timeout-ms") {
            valid = ParseInteger(value, 1, 600000, &parsed.options.push_timeout_ms);
        } else if (name == "content-size") {
            valid = ParseInteger(value, 1, 4096, &parsed.options.content_size);
        } else if (name == "verbose") {
            valid = ParseBool(value, &parsed.options.verbose);
        } else {
            parsed.error = "unknown option: " + arg;
            return parsed;
        }

        if (!valid) {
            parsed.error = "invalid value for --" + name + ": " + value;
            return parsed;
        }
    }

    if (parsed.options.backend == StressBackend::kReal && parsed.options.config_path.empty()) {
        parsed.error = "--config is required when --backend=real";
        return parsed;
    }
    if (!parsed.options.prepare_users && parsed.options.run_id.empty()) {
        parsed.error = "--run-id is required when --prepare-users=false";
        return parsed;
    }
    if (parsed.options.run_id.empty()) {
        parsed.options.run_id = GenerateRunId();
    }
    if (!IsValidRunId(parsed.options.run_id)) {
        parsed.error = "invalid --run-id: " + parsed.options.run_id;
        return parsed;
    }

    parsed.ok = true;
    return parsed;
}

class FastPasswordHasher : public chat::IPasswordHasher {
   public:
    std::optional<std::string> Hash(const std::string& password) const override {
        return EncodeFakePasswordHash(password);
    }

    bool Verify(const std::string& password, const std::string& encoded_hash) const override {
        return encoded_hash == EncodeFakePasswordHash(password);
    }

    bool NeedsRehash(const std::string& encoded_hash) const override {
        (void)encoded_hash;
        return false;
    }
};

class StressUserRepository : public chat::IUserRepository {
   public:
    explicit StressUserRepository(const std::vector<StressUserPair>& pairs) {
        for (const StressUserPair& pair : pairs) {
            AddUser(pair.alice_id, pair.alice_username, pair.alice_username);
            AddUser(pair.bob_id, pair.bob_username, pair.bob_username);
        }
    }

    chat::FindUserResult findByUsername(const std::string& username) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = users_by_name_.find(username);
        if (it == users_by_name_.end()) {
            return chat::FindUserResult{chat::RepositoryStatus::kNotFound, std::nullopt};
        }
        return chat::FindUserResult{chat::RepositoryStatus::kOk, it->second};
    }

    chat::FindUserResult findById(chat::UserId user_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = users_by_id_.find(user_id);
        if (it == users_by_id_.end()) {
            return chat::FindUserResult{chat::RepositoryStatus::kNotFound, std::nullopt};
        }
        return chat::FindUserResult{chat::RepositoryStatus::kOk, it->second};
    }

    chat::CreateUserResult createUser(const std::string& username, const std::string& password_hash,
                                      const std::string& nickname) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (users_by_name_.find(username) != users_by_name_.end()) {
            return chat::CreateUserResult{chat::RepositoryStatus::kDuplicate, 0};
        }

        chat::UserRecord record;
        record.id = next_user_id_++;
        record.username = username;
        record.password_hash = password_hash;
        record.nickname = nickname;
        record.status = 1;
        record.created_at = "2026-01-01 00:00:00";
        record.updated_at = "2026-01-01 00:00:00";
        users_by_id_[record.id] = record;
        users_by_name_[record.username] = record;
        return chat::CreateUserResult{chat::RepositoryStatus::kOk, record.id};
    }

    chat::RepositoryStatus updatePasswordHash(chat::UserId user_id, const std::string& password_hash) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id_it = users_by_id_.find(user_id);
        if (id_it == users_by_id_.end()) {
            return chat::RepositoryStatus::kNotFound;
        }
        id_it->second.password_hash = password_hash;
        users_by_name_[id_it->second.username] = id_it->second;
        return chat::RepositoryStatus::kOk;
    }

   private:
    void AddUser(chat::UserId user_id, const std::string& username, const std::string& nickname) {
        chat::UserRecord record;
        record.id = user_id;
        record.username = username;
        record.password_hash = EncodeFakePasswordHash(kPassword);
        record.nickname = nickname;
        record.status = 1;
        record.created_at = "2026-01-01 00:00:00";
        record.updated_at = "2026-01-01 00:00:00";
        users_by_id_[record.id] = record;
        users_by_name_[record.username] = record;
        next_user_id_ = std::max(next_user_id_, record.id + 1);
    }

    std::mutex mutex_;
    std::unordered_map<chat::UserId, chat::UserRecord> users_by_id_;
    std::unordered_map<std::string, chat::UserRecord> users_by_name_;
    chat::UserId next_user_id_ = kFirstStressUserId;
};

class StressMessageRepository : public chat::IMessageRepository {
   public:
    chat::FindOrCreateConversationResult findOrCreateSingleConversation(chat::UserId user_a,
                                                                        chat::UserId user_b) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string key = PairKey(user_a, user_b);
        const auto existing = conversations_by_pair_.find(key);
        if (existing != conversations_by_pair_.end()) {
            return chat::FindOrCreateConversationResult{chat::RepositoryStatus::kOk, existing->second, false};
        }
        const std::string conversation_id = ConversationId(user_a, user_b);
        conversations_by_pair_[key] = conversation_id;
        return chat::FindOrCreateConversationResult{chat::RepositoryStatus::kOk, conversation_id, true};
    }

    chat::CreateMessageResult createMessage(const chat::MessageRecord& message) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string client_key = ClientKey(message.from_user_id, message.client_msg_id);
        const auto existing_id = message_id_by_client_key_.find(client_key);
        if (existing_id != message_id_by_client_key_.end()) {
            const auto existing = messages_by_id_.find(existing_id->second);
            if (existing == messages_by_id_.end()) {
                return chat::CreateMessageResult{chat::RepositoryStatus::kQueryFailed, "", std::nullopt, false};
            }
            return chat::CreateMessageResult{chat::RepositoryStatus::kOk, existing->second.id, existing->second, false};
        }

        chat::MessageRecord stored = message;
        if (stored.conversation_id.empty()) {
            stored.conversation_id = ConversationId(stored.from_user_id, stored.to_user_id);
        }
        if (stored.id.empty()) {
            stored.id = "msg_stress_" + std::to_string(next_generated_message_id_++);
        }
        if (stored.sequence <= 0) {
            stored.sequence = ++next_sequence_by_conversation_[stored.conversation_id];
        } else {
            auto& next_sequence = next_sequence_by_conversation_[stored.conversation_id];
            next_sequence = std::max(next_sequence, stored.sequence);
        }

        messages_by_id_[stored.id] = stored;
        message_order_.push_back(stored.id);
        message_id_by_client_key_[client_key] = stored.id;
        return chat::CreateMessageResult{chat::RepositoryStatus::kOk, stored.id, stored, true};
    }

    chat::FindMessageResult findMessageByClientMsgId(chat::UserId from_user_id,
                                                     const std::string& client_msg_id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing_id = message_id_by_client_key_.find(ClientKey(from_user_id, client_msg_id));
        if (existing_id == message_id_by_client_key_.end()) {
            return chat::FindMessageResult{chat::RepositoryStatus::kNotFound, std::nullopt};
        }
        const auto existing = messages_by_id_.find(existing_id->second);
        if (existing == messages_by_id_.end()) {
            return chat::FindMessageResult{chat::RepositoryStatus::kQueryFailed, std::nullopt};
        }
        return chat::FindMessageResult{chat::RepositoryStatus::kOk, existing->second};
    }

    chat::ListOfflineMessagesResult listOfflineMessages(chat::UserId to_user_id, int32_t limit,
                                                        const std::string& cursor) override {
        std::lock_guard<std::mutex> lock(mutex_);
        chat::ListOfflineMessagesResult result;
        result.status = chat::RepositoryStatus::kOk;
        bool after_cursor = cursor.empty();
        for (const std::string& message_id : message_order_) {
            const auto existing = messages_by_id_.find(message_id);
            if (existing == messages_by_id_.end()) {
                continue;
            }
            if (!after_cursor) {
                if (existing->second.id == cursor) {
                    after_cursor = true;
                }
                continue;
            }
            if (existing->second.to_user_id != to_user_id || existing->second.status != chat::MessageStatus::kStored) {
                continue;
            }
            if (static_cast<int32_t>(result.messages.size()) >= limit) {
                result.has_more = true;
                break;
            }
            result.messages.push_back(existing->second);
        }
        return result;
    }

    chat::MarkDeliveredResult markDelivered(chat::UserId to_user_id,
                                            const std::vector<std::string>& message_ids) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t affected_rows = 0;
        for (const std::string& message_id : message_ids) {
            auto existing = messages_by_id_.find(message_id);
            if (existing == messages_by_id_.end() || existing->second.to_user_id != to_user_id) {
                continue;
            }
            if (!chat::CanTransitionMessageStatus(existing->second.status, chat::MessageStatus::kDelivered)) {
                continue;
            }
            if (existing->second.status == chat::MessageStatus::kStored) {
                existing->second.status = chat::MessageStatus::kDelivered;
                existing->second.delivered_at = NowSeconds();
            }
            ++affected_rows;
        }
        return chat::MarkDeliveredResult{chat::RepositoryStatus::kOk, affected_rows};
    }

    chat::MarkReadResult markRead(chat::UserId to_user_id, const std::vector<std::string>& message_ids) override {
        std::lock_guard<std::mutex> lock(mutex_);
        int32_t affected_rows = 0;
        for (const std::string& message_id : message_ids) {
            auto existing = messages_by_id_.find(message_id);
            if (existing == messages_by_id_.end() || existing->second.to_user_id != to_user_id) {
                continue;
            }
            if (!chat::CanTransitionMessageStatus(existing->second.status, chat::MessageStatus::kRead)) {
                continue;
            }
            if (existing->second.status != chat::MessageStatus::kRead) {
                existing->second.status = chat::MessageStatus::kRead;
                existing->second.read_at = NowSeconds();
            }
            ++affected_rows;
        }
        return chat::MarkReadResult{chat::RepositoryStatus::kOk, affected_rows};
    }

   private:
    std::string ClientKey(chat::UserId from_user_id, const std::string& client_msg_id) const {
        return std::to_string(from_user_id) + ":" + client_msg_id;
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::string> conversations_by_pair_;
    std::unordered_map<std::string, int64_t> next_sequence_by_conversation_;
    std::unordered_map<std::string, chat::MessageRecord> messages_by_id_;
    std::unordered_map<std::string, std::string> message_id_by_client_key_;
    std::vector<std::string> message_order_;
    uint64_t next_generated_message_id_ = 1;
};

struct LatencySamples {
    std::vector<double> login_ms;
    std::vector<double> send_ms;
    std::vector<double> push_ms;
    std::vector<double> ack_ms;
};

struct StressDurations {
    double login_duration_ms = 0.0;
    double message_duration_ms = 0.0;
};

class StressMetrics {
   public:
    void AddLoginLatency(double ms) { AddSample(&LatencySamples::login_ms, ms); }
    void AddSendLatency(double ms) { AddSample(&LatencySamples::send_ms, ms); }
    void AddPushLatency(double ms) { AddSample(&LatencySamples::push_ms, ms); }
    void AddAckLatency(double ms) { AddSample(&LatencySamples::ack_ms, ms); }

    LatencySamples SnapshotSamples() const {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        return samples_;
    }

    void AddError(const std::string& category, const std::string& detail) {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        if (errors_.size() < kMaxStoredErrors) {
            errors_.push_back(category + ": " + detail);
        }
    }

    std::vector<std::string> SnapshotErrors() const {
        std::lock_guard<std::mutex> lock(errors_mutex_);
        return errors_;
    }

    int64_t ErrorCount() const {
        return config_errors.load() + prepare_errors.load() + mysql_init_errors.load() + redis_init_errors.load() +
               connect_errors.load() + login_errors.load() + send_errors.load() + lost_pushes.load() +
               duplicate_pushes.load() + push_validation_errors.load() + ack_errors.load() + protocol_errors.load() +
               server_errors.load();
    }

    std::atomic<int64_t> config_errors{0};
    std::atomic<int64_t> prepare_errors{0};
    std::atomic<int64_t> mysql_init_errors{0};
    std::atomic<int64_t> redis_init_errors{0};
    std::atomic<int64_t> login_success{0};
    std::atomic<int64_t> login_errors{0};
    std::atomic<int64_t> send_success{0};
    std::atomic<int64_t> send_errors{0};
    std::atomic<int64_t> pushes_received{0};
    std::atomic<int64_t> lost_pushes{0};
    std::atomic<int64_t> duplicate_pushes{0};
    std::atomic<int64_t> push_validation_errors{0};
    std::atomic<int64_t> ack_success{0};
    std::atomic<int64_t> ack_errors{0};
    std::atomic<int64_t> protocol_errors{0};
    std::atomic<int64_t> connect_errors{0};
    std::atomic<int64_t> server_errors{0};

   private:
    void AddSample(std::vector<double> LatencySamples::*member, double ms) {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        (samples_.*member).push_back(ms);
    }

    mutable std::mutex samples_mutex_;
    LatencySamples samples_;
    mutable std::mutex errors_mutex_;
    std::vector<std::string> errors_;
};

double ElapsedMs(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

double Percentile(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double rank = percentile / 100.0 * static_cast<double>(values.size() - 1);
    const std::size_t index = static_cast<std::size_t>(std::round(rank));
    return values[std::min(index, values.size() - 1)];
}

class StressClientConnection {
   public:
    StressClientConnection() = default;

    ~StressClientConnection() { Close(); }

    StressClientConnection(const StressClientConnection&) = delete;
    StressClientConnection& operator=(const StressClientConnection&) = delete;

    bool Connect(const std::string& host, int port, int timeout_ms, std::string* error) {
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            *error = "socket failed: " + std::string(std::strerror(errno));
            return false;
        }

        const int flags = fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            *error = "set nonblocking failed: " + std::string(std::strerror(errno));
            Close();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            *error = "invalid host: " + host;
            Close();
            return false;
        }

        const int connect_result = connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (connect_result < 0 && errno != EINPROGRESS) {
            *error = "connect failed: " + std::string(std::strerror(errno));
            Close();
            return false;
        }
        if (connect_result < 0) {
            pollfd pfd{};
            pfd.fd = fd_;
            pfd.events = POLLOUT;
            const int poll_result = poll(&pfd, 1, timeout_ms);
            if (poll_result <= 0) {
                *error = poll_result == 0 ? "connect timed out" : "connect poll failed";
                Close();
                return false;
            }
            int socket_error = 0;
            socklen_t socket_error_len = sizeof(socket_error);
            if (getsockopt(fd_, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) != 0 || socket_error != 0) {
                *error = "connect completion failed: " +
                         std::string(std::strerror(socket_error == 0 ? errno : socket_error));
                Close();
                return false;
            }
        }

        if (fcntl(fd_, F_SETFL, flags) < 0) {
            *error = "restore socket flags failed: " + std::string(std::strerror(errno));
            Close();
            return false;
        }

        timeval timeout{};
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        const int nodelay = 1;
        if (setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
            *error = "setsockopt TCP_NODELAY failed: " + std::string(std::strerror(errno));
            Close();
            return false;
        }

        reader_ = std::thread([this]() { ReadLoop(); });
        return true;
    }

    bool SendRequestAndWait(const std::string& msg_type, chat::SeqId seq, const std::string& token,
                            const nlohmann::json& data, int timeout_ms, nlohmann::json* response, std::string* error) {
        nlohmann::json request;
        request["msg_type"] = msg_type;
        request["seq"] = seq;
        request["token"] = token;
        request["data"] = data;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_responses_.insert(seq);
        }

        const std::string packet = write_codec_.encode(request.dump());
        if (!SendAll(packet, error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_responses_.erase(seq);
            return false;
        }

        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        const bool ready = cv_.wait_until(lock, deadline, [&]() {
            return responses_.find(seq) != responses_.end() || !protocol_errors_.empty() || closed_;
        });
        if (!ready || responses_.find(seq) == responses_.end()) {
            pending_responses_.erase(seq);
            *error = ready && !protocol_errors_.empty() ? protocol_errors_.front() : "response timed out";
            return false;
        }

        *response = responses_[seq];
        responses_.erase(seq);
        pending_responses_.erase(seq);
        return true;
    }

    bool WaitForPush(int timeout_ms, nlohmann::json* push, std::string* error) {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        const bool ready =
            cv_.wait_until(lock, deadline, [&]() { return !pushes_.empty() || !protocol_errors_.empty() || closed_; });
        if (!ready || pushes_.empty()) {
            *error = ready && !protocol_errors_.empty() ? protocol_errors_.front() : "push timed out";
            return false;
        }
        *push = std::move(pushes_.front());
        pushes_.pop_front();
        return true;
    }

    int SyncPushCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sync_push_count_;
    }

    void Close() {
        int old_fd = -1;
        {
            std::lock_guard<std::mutex> lock(close_mutex_);
            old_fd = fd_;
            fd_ = -1;
            stopping_.store(true);
        }
        if (old_fd >= 0) {
            shutdown(old_fd, SHUT_RDWR);
            close(old_fd);
        }
        if (reader_.joinable()) {
            reader_.join();
        }
    }

   private:
    bool SendAll(const std::string& packet, std::string* error) {
        std::lock_guard<std::mutex> lock(send_mutex_);
        std::size_t offset = 0;
        while (offset < packet.size()) {
            const ssize_t sent = send(fd_, packet.data() + offset, packet.size() - offset, 0);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            *error = "send failed: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }

    void ReadLoop() {
        char buffer[4096];
        while (!stopping_.load()) {
            const ssize_t received = recv(fd_, buffer, sizeof(buffer), 0);
            if (received > 0) {
                std::vector<std::string> packets;
                if (!read_codec_.feed(std::string(buffer, static_cast<std::size_t>(received)), packets)) {
                    RecordProtocolError("packet exceeded maximum size");
                    break;
                }
                for (const std::string& packet : packets) {
                    RoutePacket(packet);
                }
                continue;
            }
            if (received == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            if (!stopping_.load()) {
                RecordProtocolError("recv failed: " + std::string(std::strerror(errno)));
            }
            break;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    void RoutePacket(const std::string& packet) {
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(packet);
        } catch (const nlohmann::json::exception& e) {
            RecordProtocolError(std::string("json parse failed: ") + e.what());
            return;
        }
        if (!parsed.is_object() || !parsed.contains("msg_type") || !parsed["msg_type"].is_string() ||
            !parsed.contains("seq") || !parsed["seq"].is_number_unsigned()) {
            RecordProtocolError("invalid response envelope");
            return;
        }

        const std::string msg_type = parsed["msg_type"].get<std::string>();
        const chat::SeqId seq = parsed["seq"].get<chat::SeqId>();
        std::lock_guard<std::mutex> lock(mutex_);
        if (seq > 0) {
            if (pending_responses_.find(seq) == pending_responses_.end()) {
                protocol_errors_.push_back("response routed to no waiter: seq=" + std::to_string(seq));
            } else {
                responses_[seq] = std::move(parsed);
            }
            cv_.notify_all();
            return;
        }

        if (msg_type == "message_push") {
            pushes_.push_back(std::move(parsed));
        } else if (msg_type == "message_sync_push") {
            ++sync_push_count_;
        } else {
            protocol_errors_.push_back("unexpected async message type: " + msg_type);
        }
        cv_.notify_all();
    }

    void RecordProtocolError(const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_errors_.push_back(error);
        cv_.notify_all();
    }

    int fd_ = -1;
    std::atomic<bool> stopping_{false};
    std::thread reader_;
    chat::PacketCodec read_codec_;
    chat::PacketCodec write_codec_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_set<chat::SeqId> pending_responses_;
    std::unordered_map<chat::SeqId, nlohmann::json> responses_;
    std::deque<nlohmann::json> pushes_;
    std::vector<std::string> protocol_errors_;
    int sync_push_count_ = 0;
    bool closed_ = false;

    std::mutex send_mutex_;
    std::mutex close_mutex_;
};

bool ExpectEnvelope(const nlohmann::json& value, const std::string& msg_type, chat::SeqId seq, chat::ErrorCode code,
                    std::string* error) {
    if (!value.is_object() || !value.contains("msg_type") || !value.contains("seq") || !value.contains("code") ||
        !value.contains("message") || !value.contains("data")) {
        *error = "missing response envelope field";
        return false;
    }
    if (value["msg_type"].get<std::string>() != msg_type) {
        *error = "unexpected msg_type: " + value["msg_type"].get<std::string>();
        return false;
    }
    if (value["seq"].get<chat::SeqId>() != seq) {
        *error = "unexpected seq: " + std::to_string(value["seq"].get<chat::SeqId>());
        return false;
    }
    if (value["code"].get<int>() != static_cast<int>(code)) {
        *error =
            "unexpected code: " + std::to_string(value["code"].get<int>()) + " message=" + value.value("message", "");
        return false;
    }
    if (!value["data"].is_object()) {
        *error = "data is not object";
        return false;
    }
    return true;
}

bool ConnectReadinessProbe(int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, kServerIp, &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }

    const int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        close(fd);
        return true;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) {
        close(fd);
        return false;
    }

    int socket_error = 0;
    socklen_t socket_error_len = sizeof(socket_error);
    const bool connected =
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_len) == 0 && socket_error == 0;
    close(fd);
    return connected;
}

bool WaitForServerReady(TcpServer* server, const std::atomic<bool>& server_done, const StressOptions& options,
                        int* actual_port, std::string* error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(options.connect_timeout_ms);
    while (Clock::now() < deadline) {
        if (server_done.load()) {
            *error = "server exited before becoming ready";
            return false;
        }
        const int port = server->getPort();
        if (port > 0 && ConnectReadinessProbe(port, std::min(100, options.connect_timeout_ms))) {
            *actual_port = port;
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    *error = "server readiness timed out";
    return false;
}

bool ValidateLoginResponse(const nlohmann::json& response, chat::SeqId seq, chat::UserId expected_user_id,
                           std::string* token, std::string* error) {
    if (!ExpectEnvelope(response, "login_resp", seq, chat::ErrorCode::OK, error)) {
        return false;
    }
    const nlohmann::json& data = response["data"];
    if (!data.contains("user_id") || data["user_id"].get<chat::UserId>() != expected_user_id) {
        *error = "login user_id mismatch";
        return false;
    }
    if (!data.contains("token") || !data["token"].is_string() || data["token"].get<std::string>().empty()) {
        *error = "login token missing";
        return false;
    }
    *token = data["token"].get<std::string>();
    return true;
}

bool ValidateSendResponse(const nlohmann::json& response, chat::SeqId seq, chat::UserId expected_to_user_id,
                          std::string* message_id, std::string* conversation_id, std::string* error) {
    if (!ExpectEnvelope(response, "send_message_resp", seq, chat::ErrorCode::OK, error)) {
        return false;
    }
    const nlohmann::json& data = response["data"];
    if (!data.contains("message_id") || !data["message_id"].is_string() ||
        data["message_id"].get<std::string>().empty()) {
        *error = "send response message_id missing";
        return false;
    }
    if (!data.contains("conversation_id") || !data["conversation_id"].is_string() ||
        data["conversation_id"].get<std::string>().empty()) {
        *error = "send response conversation_id missing";
        return false;
    }
    if (!data.contains("to_user_id") || data["to_user_id"].get<chat::UserId>() != expected_to_user_id) {
        *error = "send response to_user_id mismatch";
        return false;
    }
    *message_id = data["message_id"].get<std::string>();
    *conversation_id = data["conversation_id"].get<std::string>();
    return true;
}

bool ValidatePush(const nlohmann::json& push, const std::string& message_id, const std::string& conversation_id,
                  chat::UserId from_user_id, chat::UserId to_user_id, const std::string& content, std::string* error) {
    if (!ExpectEnvelope(push, "message_push", 0, chat::ErrorCode::OK, error)) {
        return false;
    }
    const nlohmann::json& data = push["data"];
    if (data.value("message_id", "") != message_id) {
        *error = "push message_id mismatch";
        return false;
    }
    if (data.value("conversation_id", "") != conversation_id) {
        *error = "push conversation_id mismatch";
        return false;
    }
    if (!data.contains("from_user_id") || data["from_user_id"].get<chat::UserId>() != from_user_id) {
        *error = "push from_user_id mismatch";
        return false;
    }
    if (!data.contains("to_user_id") || data["to_user_id"].get<chat::UserId>() != to_user_id) {
        *error = "push to_user_id mismatch";
        return false;
    }
    if (data.value("content", "") != content) {
        *error = "push content mismatch";
        return false;
    }
    return true;
}

bool ValidateAckResponse(const nlohmann::json& response, chat::SeqId seq, const std::string& message_id,
                         std::string* error) {
    if (!ExpectEnvelope(response, "message_ack_resp", seq, chat::ErrorCode::OK, error)) {
        return false;
    }
    const nlohmann::json& data = response["data"];
    if (!data.contains("message_ids") || !data["message_ids"].is_array()) {
        *error = "ack message_ids missing";
        return false;
    }
    bool found = false;
    for (const auto& id : data["message_ids"]) {
        if (id.is_string() && id.get<std::string>() == message_id) {
            found = true;
            break;
        }
    }
    if (!found) {
        *error = "ack message_id missing from response";
        return false;
    }
    if (!data.contains("affected_rows") || !data["affected_rows"].is_number_integer() ||
        data["affected_rows"].get<int>() <= 0) {
        *error = "ack affected_rows invalid";
        return false;
    }
    return true;
}

void RecordConnectError(StressMetrics* metrics, const std::string& detail) {
    ++metrics->connect_errors;
    metrics->AddError("connect_errors", detail);
}

void RecordLoginError(StressMetrics* metrics, const std::string& detail) {
    ++metrics->login_errors;
    metrics->AddError("login_errors", detail);
}

void RecordSendError(StressMetrics* metrics, const std::string& detail) {
    ++metrics->send_errors;
    metrics->AddError("send_errors", detail);
}

void RecordPushError(StressMetrics* metrics, const std::string& detail) { metrics->AddError("push", detail); }

void RecordAckError(StressMetrics* metrics, const std::string& detail) {
    ++metrics->ack_errors;
    metrics->AddError("ack_errors", detail);
}

bool LoginClient(StressClientConnection* client, const std::string& username, chat::UserId user_id, chat::SeqId* seq,
                 const StressOptions& options, StressMetrics* metrics, std::string* token) {
    nlohmann::json data;
    data["username"] = username;
    data["password"] = kPassword;

    nlohmann::json response;
    std::string error;
    const chat::SeqId request_seq = (*seq)++;
    const auto started = Clock::now();
    if (!client->SendRequestAndWait("login", request_seq, "", data, options.request_timeout_ms, &response, &error)) {
        RecordLoginError(metrics, username + ": " + error);
        return false;
    }
    if (!ValidateLoginResponse(response, request_seq, user_id, token, &error)) {
        RecordLoginError(metrics, username + ": " + error);
        return false;
    }

    ++metrics->login_success;
    metrics->AddLoginLatency(ElapsedMs(started, Clock::now()));
    return true;
}

void RunPair(int pair_index, int port, const StressOptions& options, StressMetrics* metrics,
             std::mutex* connect_mutex) {
    const StressUserPair pair{pair_index, StressUsername(options.run_id, "alice", pair_index),
                              StressUsername(options.run_id, "bob", pair_index), AliceUserId(pair_index),
                              BobUserId(pair_index)};
    StressClientConnection alice;
    StressClientConnection bob;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(*connect_mutex);
        if (!alice.Connect(kServerIp, port, options.connect_timeout_ms, &error)) {
            RecordConnectError(metrics, "alice_" + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (!bob.Connect(kServerIp, port, options.connect_timeout_ms, &error)) {
            RecordConnectError(metrics, "bob_" + std::to_string(pair_index) + ": " + error);
            return;
        }
    }

    chat::SeqId alice_seq = 1;
    chat::SeqId bob_seq = 1;
    std::string alice_token;
    std::string bob_token;
    if (!LoginClient(&alice, pair.alice_username, pair.alice_id, &alice_seq, options, metrics, &alice_token)) {
        return;
    }
    if (!LoginClient(&bob, pair.bob_username, pair.bob_id, &bob_seq, options, metrics, &bob_token)) {
        return;
    }

    std::unordered_set<std::string> seen_pushes;
    for (int message_index = 0; message_index < options.messages_per_pair; ++message_index) {
        const std::string content = MakeContent(pair_index, message_index, options.content_size);

        nlohmann::json send_data;
        send_data["client_msg_id"] = MakeClientMessageId(options.run_id, "measure", pair.pair_index, message_index);
        send_data["to_user_id"] = pair.bob_id;
        send_data["content"] = content;

        nlohmann::json send_response;
        std::string message_id;
        std::string conversation_id;
        const chat::SeqId send_seq = alice_seq++;
        const auto send_started = Clock::now();
        if (!alice.SendRequestAndWait("send_message", send_seq, alice_token, send_data, options.request_timeout_ms,
                                      &send_response, &error)) {
            RecordSendError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (!ValidateSendResponse(send_response, send_seq, pair.bob_id, &message_id, &conversation_id, &error)) {
            RecordSendError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }

        ++metrics->send_success;
        metrics->AddSendLatency(ElapsedMs(send_started, Clock::now()));

        nlohmann::json push;
        if (!bob.WaitForPush(options.push_timeout_ms, &push, &error)) {
            ++metrics->lost_pushes;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (seen_pushes.find(push["data"].value("message_id", "")) != seen_pushes.end()) {
            ++metrics->duplicate_pushes;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": duplicate push");
            return;
        }
        if (!ValidatePush(push, message_id, conversation_id, pair.alice_id, pair.bob_id, content, &error)) {
            ++metrics->push_validation_errors;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        seen_pushes.insert(message_id);
        ++metrics->pushes_received;
        metrics->AddPushLatency(ElapsedMs(send_started, Clock::now()));

        nlohmann::json ack_data;
        ack_data["message_id"] = message_id;
        nlohmann::json ack_response;
        const chat::SeqId ack_seq = bob_seq++;
        const auto ack_started = Clock::now();
        if (!bob.SendRequestAndWait("message_ack", ack_seq, bob_token, ack_data, options.request_timeout_ms,
                                    &ack_response, &error)) {
            RecordAckError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (!ValidateAckResponse(ack_response, ack_seq, message_id, &error)) {
            RecordAckError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        ++metrics->ack_success;
        metrics->AddAckLatency(ElapsedMs(ack_started, Clock::now()));
    }
}

void RunClientWorkers(int port, const StressOptions& options, StressMetrics* metrics) {
    std::atomic<int> next_pair{0};
    std::mutex connect_mutex;
    const int worker_count = std::min(options.client_workers, options.pairs);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));
    for (int worker_index = 0; worker_index < worker_count; ++worker_index) {
        workers.emplace_back([&, worker_index]() {
            while (true) {
                const int pair_index = next_pair.fetch_add(1);
                if (pair_index >= options.pairs) {
                    break;
                }
                if (options.verbose) {
                    std::cerr << "[worker " << worker_index << "] pair " << pair_index << "\n";
                }
                RunPair(pair_index, port, options, metrics, &connect_mutex);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}

void PrintLatencyLine(const std::string& name, const std::vector<double>& values) {
    std::cout << "  " << name << "_p50_ms=" << std::fixed << std::setprecision(2) << Percentile(values, 50.0) << " "
              << name << "_p95_ms=" << Percentile(values, 95.0) << "\n";
}

void PrintReport(const StressOptions& options, const StressMetrics& metrics, const StressDurations& durations,
                 const std::optional<chat::ServerConfig>& config) {
    const int64_t expected_messages = static_cast<int64_t>(options.pairs) * options.messages_per_pair;
    const LatencySamples samples = metrics.SnapshotSamples();
    const double message_duration_seconds = durations.message_duration_ms / 1000.0;
    const double messages_per_second =
        message_duration_seconds > 0.0 ? static_cast<double>(expected_messages) / message_duration_seconds : 0.0;

    std::cout << "\nBusiness stress test report\n"
              << "  backend=" << BackendName(options.backend) << "\n"
              << "  run_id=" << options.run_id << "\n"
              << "  password_hasher=" << PasswordHasherName(options.password_hasher) << "\n";
    if (options.password_hasher == PasswordHasherMode::kFast) {
        std::cout << "  login_latency_note=fast hasher does not include production bcrypt cost\n";
    }
    if (config.has_value()) {
        std::cout << "  config_server_port_ignored=" << config->server.listen_port << "\n"
                  << "  mysql=" << config->mysql.host << ":" << config->mysql.port << "/" << config->mysql.database
                  << " pool=" << config->mysql_pool.min_connections << ".." << config->mysql_pool.max_connections
                  << "\n"
                  << "  redis_enabled=" << (config->redis.enabled ? "true" : "false") << "\n";
        if (config->redis.enabled) {
            std::cout << "  redis=" << config->redis.host << ":" << config->redis.port << "/" << config->redis.database
                      << "\n"
                      << "  redis_key_prefix_effective=" << config->redis.key_prefix << "\n";
        }
    }
    std::cout << "  pairs=" << options.pairs << " messages_per_pair=" << options.messages_per_pair
              << " client_workers=" << options.client_workers << "\n"
              << "  expected_messages=" << expected_messages << "\n"
              << "  config_errors=" << metrics.config_errors.load()
              << " prepare_errors=" << metrics.prepare_errors.load()
              << " mysql_init_errors=" << metrics.mysql_init_errors.load()
              << " redis_init_errors=" << metrics.redis_init_errors.load() << "\n"
              << "  login_success=" << metrics.login_success.load() << " login_errors=" << metrics.login_errors.load()
              << "\n"
              << "  send_success=" << metrics.send_success.load() << " send_errors=" << metrics.send_errors.load()
              << "\n"
              << "  push_received=" << metrics.pushes_received.load() << " lost_push=" << metrics.lost_pushes.load()
              << " duplicate_push=" << metrics.duplicate_pushes.load()
              << " push_validation_errors=" << metrics.push_validation_errors.load() << "\n"
              << "  ack_success=" << metrics.ack_success.load() << " ack_errors=" << metrics.ack_errors.load() << "\n"
              << "  connect_errors=" << metrics.connect_errors.load()
              << " protocol_errors=" << metrics.protocol_errors.load()
              << " server_errors=" << metrics.server_errors.load() << "\n";
    PrintLatencyLine("login", samples.login_ms);
    PrintLatencyLine("send", samples.send_ms);
    PrintLatencyLine("push", samples.push_ms);
    PrintLatencyLine("ack", samples.ack_ms);
    std::cout << "  login_duration_ms=" << std::fixed << std::setprecision(2) << durations.login_duration_ms << "\n"
              << "  message_duration_ms=" << durations.message_duration_ms << "\n"
              << "  messages_per_second=" << messages_per_second << "\n";

    const std::vector<std::string> errors = metrics.SnapshotErrors();
    if (!errors.empty()) {
        std::cout << "  first_errors:\n";
        for (const std::string& error : errors) {
            std::cout << "    - " << error << "\n";
        }
    }
}

bool Passed(const StressOptions& options, const StressMetrics& metrics) {
    const int64_t expected_messages = static_cast<int64_t>(options.pairs) * options.messages_per_pair;
    const int64_t expected_logins = static_cast<int64_t>(options.pairs) * 2;
    return metrics.login_success.load() == expected_logins && metrics.send_success.load() == expected_messages &&
           metrics.pushes_received.load() == expected_messages && metrics.ack_success.load() == expected_messages &&
           metrics.lost_pushes.load() == 0 && metrics.duplicate_pushes.load() == 0 &&
           metrics.push_validation_errors.load() == 0 && metrics.ErrorCount() == 0;
}

int RunStress(const StressOptions& options) {
    const std::vector<StressUserPair> user_pairs = BuildMemoryUserPairs(options.run_id, options.pairs);
    StressUserRepository user_repository(user_pairs);
    StressMessageRepository message_repository;
    chat::SessionManager session_manager;
    FastPasswordHasher password_hasher;
    chat::UserService user_service(user_repository, session_manager, nullptr, nullptr, {}, &password_hasher);
    chat::ChatService chat_service(session_manager, message_repository, user_repository);
    chat::MessageHandler handler(user_service, chat_service);

    TcpServerTimeoutOptions timeout_options;
    timeout_options.idle_timeout_ms = 300000;
    timeout_options.heartbeat_timeout_ms = 90000;
    timeout_options.scan_interval_ms = 1000;
    TcpServer server(kServerIp, static_cast<uint16_t>(options.port), handler, timeout_options);

    StressMetrics metrics;
    StressDurations durations;
    std::atomic<bool> server_done{false};
    bool server_start_result = false;
    std::thread server_thread([&]() {
        server_start_result = server.start();
        server_done.store(true);
    });

    int actual_port = 0;
    std::string readiness_error;
    if (!WaitForServerReady(&server, server_done, options, &actual_port, &readiness_error)) {
        ++metrics.server_errors;
        metrics.AddError("server_errors", readiness_error);
        server.stop();
        if (server_thread.joinable()) {
            server_thread.join();
        }
        PrintReport(options, metrics, durations, std::nullopt);
        return 1;
    }

    const auto started = Clock::now();
    RunClientWorkers(actual_port, options, &metrics);
    durations.message_duration_ms = ElapsedMs(started, Clock::now());

    server.stop();
    if (server_thread.joinable()) {
        server_thread.join();
    }
    if (!server_start_result) {
        ++metrics.server_errors;
        metrics.AddError("server_errors", "server.start returned false");
    }

    PrintReport(options, metrics, durations, std::nullopt);
    if (!Passed(options, metrics)) {
        std::cout << "  status=FAIL\n";
        return 1;
    }
    std::cout << "  status=PASS\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const ParsedOptions parsed = ParseOptions(argc, argv);
    if (!parsed.ok) {
        std::cerr << parsed.error << "\n";
        PrintUsage(std::cerr);
        return 2;
    }
    if (parsed.help) {
        PrintUsage(std::cout);
        return 0;
    }
    return RunStress(parsed.options);
}
