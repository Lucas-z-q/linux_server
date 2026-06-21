# Real Backend Business Stress Test Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `business_stress_test` with `--backend=memory|real`, where `real` uses MySQL and optional Redis while preserving the same client workload and report shape.

**Architecture:** Keep one stress binary and one client workload. Refactor the current in-memory harness around a run-scoped user catalog, explicit server bundle lifecycle, and phase-aware metrics, then add a local real-backend factory that mirrors `src/main.cc` dependency order without changing production startup code.

**Tech Stack:** C++17, CMake, Linux sockets, TcpServer, MessageHandler, UserService, ChatService, DbPool/MySQL client, optional Redis runtime, nlohmann/json.

---

## File Structure

- Modify `tests/business_stress_test.cc`
  - Owns CLI parsing, run ID generation, user catalog, memory and real backend factories, client workload, phase metrics, reporting, and process exit status.
- Modify `CMakeLists.txt`
  - Links `business_stress_test` with the real backend sources and libraries already used by `server`.
- Create `config/stress.real.example.json`
  - Provides a safe real-backend config template without credentials.
- The implementation does not modify `src/main.cc` in this first pass.
  - The real-backend factory must keep the same dependency order and Redis conditional behavior as `src/main.cc`.

## Task 1: CLI, Run ID, and User Catalog

**Files:**

- Modify: `tests/business_stress_test.cc`

- [ ] **Step 1: Add required includes**

Add these includes to `tests/business_stress_test.cc`:

```cpp
#include <ctime>
#include <memory>
#include <variant>

#include "config/server_config.h"
```

- [ ] **Step 2: Replace the option model**

Replace the current `StressOptions` and `ParsedOptions` definitions near the top of `tests/business_stress_test.cc` with this code:

```cpp
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
```

- [ ] **Step 3: Add string conversion helpers**

Add these helpers after `EncodeFakePasswordHash`:

```cpp
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
```

- [ ] **Step 4: Add run ID helpers**

Add these helpers after the string conversion helpers:

```cpp
bool IsValidRunId(const std::string& run_id) {
    if (run_id.empty() || run_id.size() > 64) {
        return false;
    }
    for (char ch : run_id) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
                        ch == '-' || ch == '_';
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
    std::ostringstream out;
    out << "stress_" << run_id << "_" << role << "_" << std::setw(6) << std::setfill('0') << pair_index;
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
```

- [ ] **Step 5: Update usage text**

Replace `PrintUsage` with this complete function:

```cpp
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
```

- [ ] **Step 6: Extend option parsing**

In `ParseOptions`, add these branches before the existing integer branches:

```cpp
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
```

Keep the existing `pairs`, `messages-per-pair`, `client-workers`, `port`, timeout, content size, and verbose branches after this new block. At the end of `ParseOptions`, before `parsed.ok = true;`, add:

```cpp
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
```

- [ ] **Step 7: Update current username and client message ID call sites**

Replace calls to:

```cpp
UserName("alice", pair_index)
UserName("bob", pair_index)
MakeClientMessageId(pair_index, message_index)
```

with values from `StressUserPair` and `MakeClientMessageId(options.run_id, phase, pair_index, message_index)`.

Use this exact message ID for measured messages:

```cpp
const std::string client_msg_id = MakeClientMessageId(options.run_id, "measure", pair.pair_index, message_index);
```

- [ ] **Step 8: Build and verify memory help**

Run:

```bash
cmake --build build
./build/business_stress_test --help
```

Expected:

```text
Usage: business_stress_test [options]
```

- [ ] **Step 9: Verify invalid real backend config is rejected**

Run:

```bash
./build/business_stress_test --backend=real
```

Expected: exit code `2` and this text:

```text
--config is required when --backend=real
```

- [ ] **Step 10: Commit**

Run:

```bash
git add tests/business_stress_test.cc
git commit -m "test: add stress backend CLI options"
```

## Task 2: Phase-Aware Metrics and Report

**Files:**

- Modify: `tests/business_stress_test.cc`

- [ ] **Step 1: Add phase duration fields**

Add this struct near `LatencySamples`:

```cpp
struct StressDurations {
    double login_duration_ms = 0.0;
    double message_duration_ms = 0.0;
};
```

- [ ] **Step 2: Replace error count aggregation**

Replace `StressMetrics::ErrorCount()` with:

```cpp
    int64_t ErrorCount() const {
        return config_errors.load() + prepare_errors.load() + mysql_init_errors.load() + redis_init_errors.load() +
               connect_errors.load() + login_errors.load() + send_errors.load() + lost_pushes.load() +
               duplicate_pushes.load() + push_validation_errors.load() + ack_errors.load() + protocol_errors.load() +
               server_errors.load();
    }
```

Add these counters before `login_success`:

```cpp
    std::atomic<int64_t> config_errors{0};
    std::atomic<int64_t> prepare_errors{0};
    std::atomic<int64_t> mysql_init_errors{0};
    std::atomic<int64_t> redis_init_errors{0};
```

Remove the `push_errors` counter from `StressMetrics`.

- [ ] **Step 3: Update push error recorders**

Replace `RecordPushError` with:

```cpp
void RecordPushError(StressMetrics* metrics, const std::string& detail) {
    metrics->AddError("push", detail);
}
```

Keep the existing call sites that increment `lost_pushes`, `duplicate_pushes`, or `push_validation_errors` before recording the error.

- [ ] **Step 4: Replace report printing**

Change `PrintReport` signature to:

```cpp
void PrintReport(const StressOptions& options, const StressMetrics& metrics, const StressDurations& durations,
                 const std::optional<chat::ServerConfig>& config)
```

Use this throughput calculation:

```cpp
    const double message_duration_seconds = durations.message_duration_ms / 1000.0;
    const double messages_per_second =
        message_duration_seconds > 0.0 ? static_cast<double>(expected_messages) / message_duration_seconds : 0.0;
```

At the start of the report, print:

```cpp
    std::cout << "\nBusiness stress test report\n"
              << "  backend=" << BackendName(options.backend) << "\n"
              << "  run_id=" << options.run_id << "\n"
              << "  password_hasher=" << PasswordHasherName(options.password_hasher) << "\n";
    if (options.password_hasher == PasswordHasherMode::kFast) {
        std::cout << "  login_latency_note=fast hasher does not include production bcrypt cost\n";
    }
```

When `config.has_value()`, print:

```cpp
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
```

Keep the existing success/error count lines, and replace the duration lines with:

```cpp
    std::cout << "  login_duration_ms=" << std::fixed << std::setprecision(2) << durations.login_duration_ms << "\n"
              << "  message_duration_ms=" << durations.message_duration_ms << "\n"
              << "  messages_per_second=" << messages_per_second << "\n";
```

- [ ] **Step 5: Update success criteria**

Replace `Passed` with:

```cpp
bool Passed(const StressOptions& options, const StressMetrics& metrics) {
    const int64_t expected_messages = static_cast<int64_t>(options.pairs) * options.messages_per_pair;
    const int64_t expected_logins = static_cast<int64_t>(options.pairs) * 2;
    return metrics.login_success.load() == expected_logins && metrics.send_success.load() == expected_messages &&
           metrics.pushes_received.load() == expected_messages && metrics.ack_success.load() == expected_messages &&
           metrics.lost_pushes.load() == 0 && metrics.duplicate_pushes.load() == 0 &&
           metrics.push_validation_errors.load() == 0 && metrics.ErrorCount() == 0;
}
```

- [ ] **Step 6: Build and verify memory report fields**

Run:

```bash
cmake --build build
./build/business_stress_test --backend=memory --pairs=2 --messages-per-pair=1 --client-workers=1
```

Expected:

```text
backend=memory
login_duration_ms=
message_duration_ms=
messages_per_second=
status=PASS
```

- [ ] **Step 7: Commit**

Run:

```bash
git add tests/business_stress_test.cc
git commit -m "test: split stress login and message metrics"
```

## Task 3: Pair Sessions and Separated Login, Warmup, and Message Phases

**Files:**

- Modify: `tests/business_stress_test.cc`

- [ ] **Step 1: Add pair session type**

Add this class after `StressClientConnection`:

```cpp
struct StressPairSession {
    StressUserPair pair;
    StressClientConnection alice;
    StressClientConnection bob;
    chat::SeqId alice_seq = 1;
    chat::SeqId bob_seq = 1;
    std::string alice_token;
    std::string bob_token;
};
```

- [ ] **Step 2: Replace login call flow**

Replace the current `RunPair` login setup with this complete helper:

```cpp
std::unique_ptr<StressPairSession> LoginPair(const StressUserPair& pair, int port, const StressOptions& options,
                                             StressMetrics* metrics, std::mutex* connect_mutex) {
    auto session = std::make_unique<StressPairSession>();
    session->pair = pair;

    std::string error;
    {
        std::lock_guard<std::mutex> lock(*connect_mutex);
        if (!session->alice.Connect(kServerIp, port, options.connect_timeout_ms, &error)) {
            ++metrics->connect_errors;
            metrics->AddError("connect_errors", "pair " + std::to_string(pair.pair_index) + " alice: " + error);
            return nullptr;
        }
        if (!session->bob.Connect(kServerIp, port, options.connect_timeout_ms, &error)) {
            ++metrics->connect_errors;
            metrics->AddError("connect_errors", "pair " + std::to_string(pair.pair_index) + " bob: " + error);
            return nullptr;
        }
    }

    if (!LoginClient(&session->alice, pair.alice_username, pair.alice_id, &session->alice_seq, options, metrics,
                     &session->alice_token)) {
        return nullptr;
    }
    if (!LoginClient(&session->bob, pair.bob_username, pair.bob_id, &session->bob_seq, options, metrics,
                     &session->bob_token)) {
        return nullptr;
    }
    return session;
}
```

- [ ] **Step 3: Add message loop helper**

Extract the message-sending body from `RunPair` into this helper:

```cpp
void RunMessagesForPair(StressPairSession* session, const StressOptions& options, StressMetrics* metrics,
                        const std::string& phase, int message_count, bool record_success_metrics) {
    std::unordered_set<std::string> seen_pushes;
    for (int message_index = 0; message_index < message_count; ++message_index) {
        const int pair_index = session->pair.pair_index;
        const std::string content = MakeContent(pair_index, message_index, options.content_size);
        const std::string client_msg_id = MakeClientMessageId(options.run_id, phase, pair_index, message_index);

        nlohmann::json data;
        data["to_user_id"] = session->pair.bob_id;
        data["client_msg_id"] = client_msg_id;
        data["content"] = content;

        nlohmann::json send_response;
        std::string error;
        const chat::SeqId send_seq = session->alice_seq++;
        const auto send_started = Clock::now();
        if (!session->alice.SendRequestAndWait("send_message", send_seq, session->alice_token, data,
                                               options.request_timeout_ms, &send_response, &error)) {
            RecordSendError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }

        std::string message_id;
        std::string conversation_id;
        if (!ValidateSendResponse(send_response, send_seq, session->pair.bob_id, &message_id, &conversation_id,
                                  &error)) {
            RecordSendError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (record_success_metrics) {
            ++metrics->send_success;
            metrics->AddSendLatency(ElapsedMs(send_started, Clock::now()));
        }

        nlohmann::json push;
        if (!session->bob.WaitForPush(options.push_timeout_ms, &push, &error)) {
            ++metrics->lost_pushes;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (seen_pushes.find(push["data"].value("message_id", "")) != seen_pushes.end()) {
            ++metrics->duplicate_pushes;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": duplicate push");
            return;
        }
        if (!ValidatePush(push, message_id, conversation_id, session->pair.alice_id, session->pair.bob_id, content,
                          &error)) {
            ++metrics->push_validation_errors;
            RecordPushError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        seen_pushes.insert(message_id);
        if (record_success_metrics) {
            ++metrics->pushes_received;
            metrics->AddPushLatency(ElapsedMs(send_started, Clock::now()));
        }

        nlohmann::json ack_data;
        ack_data["message_id"] = message_id;
        nlohmann::json ack_response;
        const chat::SeqId ack_seq = session->bob_seq++;
        const auto ack_started = Clock::now();
        if (!session->bob.SendRequestAndWait("message_ack", ack_seq, session->bob_token, ack_data,
                                             options.request_timeout_ms, &ack_response, &error)) {
            RecordAckError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (!ValidateAckResponse(ack_response, ack_seq, message_id, &error)) {
            RecordAckError(metrics, "pair " + std::to_string(pair_index) + ": " + error);
            return;
        }
        if (record_success_metrics) {
            ++metrics->ack_success;
            metrics->AddAckLatency(ElapsedMs(ack_started, Clock::now()));
        }
    }
}
```

- [ ] **Step 4: Add session worker helpers**

Add these helpers after `RunMessagesForPair`:

```cpp
std::vector<std::unique_ptr<StressPairSession>> LoginClientWorkers(int port, const StressOptions& options,
                                                                    const std::vector<StressUserPair>& pairs,
                                                                    StressMetrics* metrics) {
    std::vector<std::unique_ptr<StressPairSession>> sessions(pairs.size());
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
                    std::cerr << "[login worker " << worker_index << "] pair " << pair_index << "\n";
                }
                sessions[static_cast<std::size_t>(pair_index)] =
                    LoginPair(pairs[static_cast<std::size_t>(pair_index)], port, options, metrics, &connect_mutex);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    return sessions;
}

void RunMessageWorkers(std::vector<std::unique_ptr<StressPairSession>>* sessions, const StressOptions& options,
                       StressMetrics* metrics, const std::string& phase, int message_count,
                       bool record_success_metrics) {
    if (message_count == 0) {
        return;
    }
    std::atomic<int> next_pair{0};
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
                StressPairSession* session = (*sessions)[static_cast<std::size_t>(pair_index)].get();
                if (session == nullptr) {
                    continue;
                }
                if (options.verbose) {
                    std::cerr << "[" << phase << " worker " << worker_index << "] pair " << pair_index << "\n";
                }
                RunMessagesForPair(session, options, metrics, phase, message_count, record_success_metrics);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
}
```

- [ ] **Step 5: Replace old client worker call in `RunStress`**

Replace the old `RunClientWorkers(actual_port, options, &metrics);` timing block with:

```cpp
    StressDurations durations;
    const auto login_started = Clock::now();
    std::vector<std::unique_ptr<StressPairSession>> sessions =
        LoginClientWorkers(actual_port, options, user_pairs, &metrics);
    durations.login_duration_ms = ElapsedMs(login_started, Clock::now());

    RunMessageWorkers(&sessions, options, &metrics, "warmup", options.warmup_messages, false);

    const auto message_started = Clock::now();
    RunMessageWorkers(&sessions, options, &metrics, "measure", options.messages_per_pair, true);
    durations.message_duration_ms = ElapsedMs(message_started, Clock::now());
```

The variable `user_pairs` is introduced in Task 4 before server construction.

- [ ] **Step 6: Build and verify phase split**

Run:

```bash
cmake --build build
./build/business_stress_test --backend=memory --pairs=4 --messages-per-pair=2 --warmup-messages=1 --client-workers=2
```

Expected:

```text
login_success=8 login_errors=0
send_success=8 send_errors=0
push_received=8 lost_push=0 duplicate_push=0 push_validation_errors=0
ack_success=8 ack_errors=0
status=PASS
```

- [ ] **Step 7: Commit**

Run:

```bash
git add tests/business_stress_test.cc
git commit -m "test: separate stress login and message phases"
```

## Task 4: Stress Server Bundle and Memory Backend Factory

**Files:**

- Modify: `tests/business_stress_test.cc`

- [ ] **Step 1: Change `StressUserRepository` constructor**

Replace the current `StressUserRepository(int pairs)` constructor with:

```cpp
    explicit StressUserRepository(const std::vector<StressUserPair>& pairs) {
        for (const StressUserPair& pair : pairs) {
            AddUser(pair.alice_id, pair.alice_username, pair.alice_username);
            AddUser(pair.bob_id, pair.bob_username, pair.bob_username);
        }
    }
```

- [ ] **Step 2: Add the memory dependency holder**

Add this struct before backend factory functions:

```cpp
struct StressServerBundle {
    chat::ServerConfig config;
    std::vector<StressUserPair> user_pairs;
    std::unique_ptr<StressUserRepository> memory_user_repository;
    std::unique_ptr<StressMessageRepository> memory_message_repository;
    std::unique_ptr<chat::SessionManager> session_manager;
    std::unique_ptr<FastPasswordHasher> fast_password_hasher;
    std::unique_ptr<chat::UserService> user_service;
    std::unique_ptr<chat::ChatService> chat_service;
    std::unique_ptr<chat::MessageHandler> handler;
    std::unique_ptr<TcpServer> server;
    std::thread server_thread;
    std::atomic<bool> server_done{false};
    bool server_start_result = false;
    int actual_port = 0;

    bool StartAndWaitReady(const StressOptions& options, std::string* error) {
        server_thread = std::thread([this]() {
            server_start_result = server->start();
            server_done.store(true);
        });
        return WaitForServerReady(server.get(), server_done, options, &actual_port, error);
    }

    void StopAndJoin() {
        if (server) {
            server->stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
};
```

- [ ] **Step 3: Add memory backend factory**

Add this function before `RunStress`:

```cpp
std::unique_ptr<StressServerBundle> CreateMemoryBackend(const StressOptions& options) {
    auto bundle = std::make_unique<StressServerBundle>();
    bundle->user_pairs = BuildMemoryUserPairs(options.run_id, options.pairs);
    bundle->memory_user_repository = std::make_unique<StressUserRepository>(bundle->user_pairs);
    bundle->memory_message_repository = std::make_unique<StressMessageRepository>();
    bundle->session_manager = std::make_unique<chat::SessionManager>();
    bundle->fast_password_hasher = std::make_unique<FastPasswordHasher>();
    bundle->user_service = std::make_unique<chat::UserService>(*bundle->memory_user_repository,
                                                               *bundle->session_manager, nullptr, nullptr,
                                                               chat::RedisConfig{}, bundle->fast_password_hasher.get());
    bundle->chat_service = std::make_unique<chat::ChatService>(*bundle->session_manager,
                                                               *bundle->memory_message_repository,
                                                               *bundle->memory_user_repository);
    bundle->handler = std::make_unique<chat::MessageHandler>(*bundle->user_service, *bundle->chat_service);

    TcpServerTimeoutOptions timeout_options;
    timeout_options.idle_timeout_ms = 300000;
    timeout_options.heartbeat_timeout_ms = 90000;
    timeout_options.scan_interval_ms = 1000;
    bundle->server = std::make_unique<TcpServer>(kServerIp, static_cast<uint16_t>(options.port), *bundle->handler,
                                                 timeout_options);
    return bundle;
}
```

- [ ] **Step 4: Rewrite `RunStress` to use the bundle**

Replace the local memory dependency construction in `RunStress` with:

```cpp
int RunStress(const StressOptions& options) {
    std::unique_ptr<StressServerBundle> bundle = CreateMemoryBackend(options);
    StressMetrics metrics;
    StressDurations durations;

    std::string readiness_error;
    if (!bundle->StartAndWaitReady(options, &readiness_error)) {
        ++metrics.server_errors;
        metrics.AddError("server_errors", readiness_error);
        bundle->StopAndJoin();
        PrintReport(options, metrics, durations, std::nullopt);
        return 1;
    }

    std::vector<std::unique_ptr<StressPairSession>> sessions;
    const auto login_started = Clock::now();
    sessions = LoginClientWorkers(bundle->actual_port, options, bundle->user_pairs, &metrics);
    durations.login_duration_ms = ElapsedMs(login_started, Clock::now());

    RunMessageWorkers(&sessions, options, &metrics, "warmup", options.warmup_messages, false);

    const auto message_started = Clock::now();
    RunMessageWorkers(&sessions, options, &metrics, "measure", options.messages_per_pair, true);
    durations.message_duration_ms = ElapsedMs(message_started, Clock::now());

    bundle->StopAndJoin();
    if (!bundle->server_start_result) {
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
```

- [ ] **Step 5: Build and verify memory backend**

Run:

```bash
cmake --build build
./build/business_stress_test --backend=memory --pairs=10 --messages-per-pair=3 --client-workers=2
```

Expected:

```text
backend=memory
login_success=20 login_errors=0
send_success=30 send_errors=0
ack_success=30 ack_errors=0
status=PASS
```

- [ ] **Step 6: Commit**

Run:

```bash
git add tests/business_stress_test.cc
git commit -m "test: add stress server bundle"
```

## Task 5: Real Backend Factory and Prepare Phase

**Files:**

- Modify: `tests/business_stress_test.cc`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add real backend includes**

Add these includes to `tests/business_stress_test.cc`:

```cpp
#include "cache/cached_user_repository.h"
#include "cache/message_dedup_cache.h"
#include "cache/redis_rate_limiter.h"
#include "common/logger.h"
#include "config/config_loader.h"
#include "config/server_config.h"
#include "db/db_pool.h"
#include "redis/redis_client.h"
#include "redis/redis_pool.h"
#include "server/redis_session_store.h"
#include "stream/redis_push_stream.h"
```

- [ ] **Step 2: Add real dependency fields to `StressServerBundle`**

Add these fields to `StressServerBundle` before the memory fields:

```cpp
    std::unique_ptr<chat::DbPool> db_pool;
    std::unique_ptr<chat::UserRepository> real_user_repository;
    std::unique_ptr<chat::CachedUserRepository> cached_user_repository;
    chat::IUserRepository* active_user_repository = nullptr;
    std::unique_ptr<chat::MessageRepository> real_message_repository;
    std::unique_ptr<chat::RedisPool> redis_pool;
    std::unique_ptr<chat::RedisClient> redis_client;
    std::unique_ptr<chat::RedisSessionStore> redis_session_store;
    std::unique_ptr<chat::RedisRateLimiter> redis_rate_limiter;
    std::unique_ptr<chat::MessageDedupCache> message_dedup_cache;
    std::unique_ptr<chat::RedisPushStream> redis_push_stream;
    std::unique_ptr<chat::BcryptPasswordHasher> bcrypt_password_hasher;
```

Update `StopAndJoin` to:

```cpp
    void StopAndJoin() {
        if (server) {
            server->stop();
        }
        if (redis_push_stream) {
            redis_push_stream->Stop();
        }
        if (server_thread.joinable()) {
            server_thread.join();
        }
        if (redis_push_stream) {
            redis_push_stream->Stop();
        }
    }
```

- [ ] **Step 3: Add config loading helper**

Add this function before `CreateMemoryBackend`:

```cpp
std::optional<chat::ServerConfig> LoadRealConfig(const StressOptions& options, StressMetrics* metrics) {
    chat::ConfigResult config_result = chat::ConfigLoader::Load(options.config_path);
    if (std::holds_alternative<chat::ConfigError>(config_result)) {
        ++metrics->config_errors;
        metrics->AddError("config_errors", std::get<chat::ConfigError>(config_result).message);
        return std::nullopt;
    }
    chat::ServerConfig config = std::get<chat::ServerConfig>(config_result);
    if (config.redis.enabled) {
        config.redis.key_prefix = config.redis.key_prefix + ":" + options.run_id;
    }
    return config;
}
```

- [ ] **Step 4: Add prepare helper**

Add this complete function before `CreateRealBackend`:

```cpp
bool LoadExistingStressUser(const std::string& username, chat::IUserRepository* repository, chat::UserId* user_id,
                            StressMetrics* metrics) {
    const chat::FindUserResult result = repository->findByUsername(username);
    if (result.status != chat::RepositoryStatus::kOk || !result.user.has_value()) {
        ++metrics->prepare_errors;
        metrics->AddError("prepare_errors", username + ": existing user not found");
        return false;
    }
    *user_id = result.user->id;
    return true;
}

bool RegisterStressUser(const std::string& username, chat::UserService* user_service, chat::UserId* user_id,
                        StressMetrics* metrics) {
    chat::RegisterRequest request{username, kPassword, username};
    chat::RegisterResult result = user_service->registerUser(request, "");
    if (result.code != chat::ErrorCode::OK) {
        ++metrics->prepare_errors;
        metrics->AddError("prepare_errors", username + ": " + result.message);
        return false;
    }
    *user_id = result.data.user_id;
    return true;
}

bool PrepareRealUsers(const StressOptions& options, chat::UserService* user_service,
                      chat::IUserRepository* active_user_repository, std::vector<StressUserPair>* user_pairs,
                      StressMetrics* metrics) {
    for (StressUserPair& pair : *user_pairs) {
        if (options.prepare_users) {
            if (!RegisterStressUser(pair.alice_username, user_service, &pair.alice_id, metrics)) {
                return false;
            }
            if (!RegisterStressUser(pair.bob_username, user_service, &pair.bob_id, metrics)) {
                return false;
            }
            continue;
        }

        if (!LoadExistingStressUser(pair.alice_username, active_user_repository, &pair.alice_id, metrics)) {
            return false;
        }
        if (!LoadExistingStressUser(pair.bob_username, active_user_repository, &pair.bob_id, metrics)) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 5: Add real backend factory**

Add this complete factory after `CreateMemoryBackend`:

```cpp
std::unique_ptr<StressServerBundle> CreateRealBackend(const StressOptions& options, chat::ServerConfig config,
                                                      StressMetrics* metrics) {
    auto bundle = std::make_unique<StressServerBundle>();
    bundle->config = config;
    bundle->user_pairs.reserve(static_cast<std::size_t>(options.pairs));
    for (int i = 0; i < options.pairs; ++i) {
        bundle->user_pairs.push_back(
            StressUserPair{i, StressUsername(options.run_id, "alice", i), StressUsername(options.run_id, "bob", i), 0,
                           0});
    }

    bundle->db_pool = std::make_unique<chat::DbPool>(bundle->config.mysql, bundle->config.mysql_pool);
    const chat::DbPoolInitResult db_result = bundle->db_pool->init();
    if (!db_result.success) {
        ++metrics->mysql_init_errors;
        metrics->AddError("mysql_init_errors", chat::DbPoolErrorToString(db_result.error));
        return nullptr;
    }

    bundle->real_user_repository = std::make_unique<chat::UserRepository>(bundle->db_pool.get());
    bundle->active_user_repository = bundle->real_user_repository.get();

    if (bundle->config.redis.enabled) {
        bundle->redis_pool = std::make_unique<chat::RedisPool>(bundle->config.redis);
        const chat::RedisPoolInitResult redis_result = bundle->redis_pool->Init();
        if (!redis_result.ok()) {
            ++metrics->redis_init_errors;
            metrics->AddError("redis_init_errors", redis_result.message);
            return nullptr;
        }
        bundle->redis_client = std::make_unique<chat::RedisClient>(bundle->redis_pool.get());
        bundle->cached_user_repository = std::make_unique<chat::CachedUserRepository>(
            bundle->real_user_repository.get(), bundle->redis_client.get(), bundle->config.redis);
        bundle->active_user_repository = bundle->cached_user_repository.get();
        bundle->redis_session_store =
            std::make_unique<chat::RedisSessionStore>(bundle->redis_client.get(), bundle->config.redis);
        bundle->redis_rate_limiter =
            std::make_unique<chat::RedisRateLimiter>(bundle->redis_client.get(), bundle->config.redis);
        bundle->message_dedup_cache =
            std::make_unique<chat::MessageDedupCache>(bundle->redis_client.get(), bundle->config.redis);
        bundle->redis_push_stream =
            std::make_unique<chat::RedisPushStream>(bundle->redis_client.get(), bundle->config.redis);
        if (!bundle->redis_push_stream->Initialize()) {
            ++metrics->redis_init_errors;
            metrics->AddError("redis_init_errors", "RedisPushStream initialization failed");
            return nullptr;
        }
    }

    bundle->real_message_repository = std::make_unique<chat::MessageRepository>(bundle->db_pool.get());
    bundle->session_manager = std::make_unique<chat::SessionManager>();
    if (options.password_hasher == PasswordHasherMode::kBcrypt) {
        bundle->bcrypt_password_hasher = std::make_unique<chat::BcryptPasswordHasher>();
    } else {
        bundle->fast_password_hasher = std::make_unique<FastPasswordHasher>();
    }
    chat::IPasswordHasher* password_hasher = options.password_hasher == PasswordHasherMode::kBcrypt
                                                 ? static_cast<chat::IPasswordHasher*>(bundle->bcrypt_password_hasher.get())
                                                 : static_cast<chat::IPasswordHasher*>(bundle->fast_password_hasher.get());

    bundle->user_service = std::make_unique<chat::UserService>(
        *bundle->active_user_repository, *bundle->session_manager, bundle->redis_session_store.get(),
        bundle->redis_rate_limiter.get(), bundle->config.redis, password_hasher);
    if (!PrepareRealUsers(options, bundle->user_service.get(), bundle->active_user_repository, &bundle->user_pairs,
                          metrics)) {
        return nullptr;
    }

    bundle->chat_service = std::make_unique<chat::ChatService>(
        *bundle->session_manager, *bundle->real_message_repository, *bundle->active_user_repository,
        bundle->redis_rate_limiter.get(), bundle->message_dedup_cache.get(), bundle->config.redis,
        bundle->redis_session_store.get(), bundle->redis_push_stream.get());
    bundle->handler = std::make_unique<chat::MessageHandler>(*bundle->user_service, *bundle->chat_service);

    TcpServerTimeoutOptions timeout_options;
    timeout_options.idle_timeout_ms = bundle->config.connection.idle_timeout_ms;
    timeout_options.heartbeat_timeout_ms = bundle->config.heartbeat.timeout_ms;
    timeout_options.scan_interval_ms = 1000;
    bundle->server = std::make_unique<TcpServer>(kServerIp, static_cast<uint16_t>(options.port), *bundle->handler,
                                                 timeout_options);

    if (bundle->redis_push_stream) {
        bundle->redis_push_stream->SetDeliveryCallback([server = bundle->server.get(), &config = bundle->config](
                                                           const chat::RemotePushEvent& event) {
            return server->deliverRemotePush(event, std::chrono::milliseconds(config.timeout.remote_push_ms));
        });
        bundle->redis_push_stream->SetMarkDeliveredCallback(
            [message_repo = bundle->real_message_repository.get()](chat::UserId user_id,
                                                                   const std::string& message_id) {
                return message_repo->markDelivered(user_id, {message_id}).status == chat::RepositoryStatus::kOk;
            });
        bundle->server->setPreShutdownHook([stream = bundle->redis_push_stream.get()]() { stream->Stop(); });
        bundle->redis_push_stream->Start();
    }

    return bundle;
}
```

- [ ] **Step 6: Select backend in `RunStress`**

At the start of `RunStress`, replace `CreateMemoryBackend(options)` with:

```cpp
    StressMetrics metrics;
    std::optional<chat::ServerConfig> real_config;
    std::unique_ptr<StressServerBundle> bundle;
    if (options.backend == StressBackend::kReal) {
        real_config = LoadRealConfig(options, &metrics);
        if (!real_config.has_value()) {
            StressDurations durations;
            PrintReport(options, metrics, durations, std::nullopt);
            std::cout << "  status=FAIL\n";
            return 1;
        }
        bundle = CreateRealBackend(options, *real_config, &metrics);
    } else {
        bundle = CreateMemoryBackend(options);
    }
    if (!bundle) {
        StressDurations durations;
        PrintReport(options, metrics, durations, real_config);
        std::cout << "  status=FAIL\n";
        return 1;
    }
```

Remove the later duplicate `StressMetrics metrics;` declaration from `RunStress`.

- [ ] **Step 7: Pass config to report**

Replace both `PrintReport(options, metrics, durations, std::nullopt);` calls after backend creation with:

```cpp
PrintReport(options, metrics, durations, real_config);
```

- [ ] **Step 8: Update CMake sources and links**

In the `business_stress_test` target in `CMakeLists.txt`, add these sources:

```cmake
    src/config/config_loader.cc
    src/db/db_pool.cc
    src/db/db_connection.cc
    src/db/mysql_statement.cc
    src/db/user_repository.cc
    src/db/message_repository.cc
```

Replace the current link line with:

```cmake
target_link_libraries(business_stress_test PRIVATE chat_logger redis_runtime security_runtime ${MYSQLCLIENT_LIB} Threads::Threads)
```

- [ ] **Step 9: Build and verify memory still passes**

Run:

```bash
cmake --build build
./build/business_stress_test --backend=memory --pairs=4 --messages-per-pair=2 --client-workers=2
```

Expected:

```text
backend=memory
status=PASS
```

- [ ] **Step 10: Verify missing real config fails cleanly**

Run:

```bash
./build/business_stress_test --backend=real --config=config/does-not-exist.json
```

Expected: exit code `1`, `status=FAIL`, and `config_errors` in `first_errors`.

- [ ] **Step 11: Commit**

Run:

```bash
git add tests/business_stress_test.cc CMakeLists.txt
git commit -m "test: add real backend stress factory"
```

## Task 6: Example Config and Real Backend Smoke Path

**Files:**

- Create: `config/stress.real.example.json`
- Modify: `tests/business_stress_test.cc`

- [ ] **Step 1: Add example config**

Create `config/stress.real.example.json` with:

```json
{
  "server": {
    "listen_ip": "127.0.0.1",
    "listen_port": 8080
  },
  "mysql": {
    "host": "127.0.0.1",
    "port": 3306,
    "username": "root",
    "password": "",
    "database": "chat_stress",
    "connect_timeout_seconds": 5,
    "read_timeout_seconds": 5,
    "write_timeout_seconds": 5,
    "pool": {
      "min_connections": 4,
      "max_connections": 16,
      "borrow_timeout_ms": 1000
    }
  },
  "redis": {
    "enabled": false,
    "host": "127.0.0.1",
    "port": 6379,
    "password": "",
    "database": 15,
    "pool_size": 16,
    "connect_timeout_ms": 500,
    "command_timeout_ms": 500,
    "key_prefix": "stress",
    "server_id": "stress-server-1",
    "session_ttl_seconds": 604800,
    "presence_ttl_seconds": 120,
    "user_cache_ttl_seconds": 300,
    "user_not_found_ttl_seconds": 30,
    "message_dedup_ttl_seconds": 86400,
    "login_rate_limit": 100000,
    "login_rate_window_seconds": 60,
    "register_rate_limit": 100000,
    "register_rate_window_seconds": 60,
    "send_rate_limit": 100000,
    "send_rate_window_seconds": 60
  },
  "log": {
    "level": "warn",
    "console": false,
    "file_path": "logs/stress.log",
    "max_size_mb": 100,
    "max_files": 5,
    "async": true
  },
  "timeout": {
    "remote_push_ms": 500
  },
  "connection": {
    "idle_timeout_ms": 300000
  },
  "heartbeat": {
    "timeout_ms": 90000
  }
}
```

- [ ] **Step 2: Print actual port**

Add `int actual_port` to the `PrintReport` signature:

```cpp
void PrintReport(const StressOptions& options, const StressMetrics& metrics, const StressDurations& durations,
                 const std::optional<chat::ServerConfig>& config, int actual_port)
```

Print:

```cpp
              << "  actual_port=" << actual_port << "\n";
```

Pass `bundle ? bundle->actual_port : 0` at each call site.

- [ ] **Step 3: Print configured Redis prefix separately**

Keep the `LoadRealConfig` mutation from Task 5:

```cpp
config.redis.key_prefix = config.redis.key_prefix + ":" + options.run_id;
```

Use a local variable in `PrintReport` to recover the configured prefix:

```cpp
const std::string suffix = ":" + options.run_id;
std::string configured_prefix = config->redis.key_prefix;
if (configured_prefix.size() > suffix.size() &&
    configured_prefix.compare(configured_prefix.size() - suffix.size(), suffix.size(), suffix) == 0) {
    configured_prefix.resize(configured_prefix.size() - suffix.size());
}
```

Print:

```cpp
                  << "  redis_key_prefix_config=" << configured_prefix << "\n"
                  << "  redis_key_prefix_effective=" << config->redis.key_prefix << "\n";
```

- [ ] **Step 4: Build and verify example config parses**

Run:

```bash
cmake --build build
./build/business_stress_test --backend=real --config=config/stress.real.example.json --pairs=1 --messages-per-pair=1
```

Expected if local MySQL is not prepared:

```text
status=FAIL
```

Expected if local MySQL is prepared:

```text
backend=real
redis_enabled=false
status=PASS
```

- [ ] **Step 5: Run the required memory regression**

Run:

```bash
./build/business_stress_test --backend=memory
```

Expected:

```text
status=PASS
```

- [ ] **Step 6: Commit**

Run:

```bash
git add tests/business_stress_test.cc config/stress.real.example.json
git commit -m "test: add real stress example config"
```

## Task 7: Final Verification

**Files:**

- Modify only if verification reveals a concrete defect in files changed by earlier tasks.

- [ ] **Step 1: Configure and build**

Run:

```bash
cmake -S . -B build
cmake --build build
```

Expected:

```text
[100%] Built target business_stress_test
```

- [ ] **Step 2: Run memory backend**

Run:

```bash
./build/business_stress_test --backend=memory
```

Expected:

```text
backend=memory
status=PASS
```

- [ ] **Step 3: Run real backend without config**

Run:

```bash
./build/business_stress_test --backend=real
```

Expected: exit code `2` and:

```text
--config is required when --backend=real
```

- [ ] **Step 4: Run real backend with missing config**

Run:

```bash
./build/business_stress_test --backend=real --config=config/missing.json
```

Expected: exit code `1`, `status=FAIL`, and a `config_errors` entry.

- [ ] **Step 5: Run real backend smoke when MySQL is available**

Prepare the database:

```bash
mysql -u root -p -e "CREATE DATABASE IF NOT EXISTS chat_stress CHARACTER SET utf8mb4;"
mysql -u root -p chat_stress < sql/001_create_chat_tables.sql
```

Run:

```bash
./build/business_stress_test \
  --backend=real \
  --config=config/stress.real.example.json \
  --pairs=2 \
  --messages-per-pair=1 \
  --client-workers=1
```

Expected:

```text
backend=real
redis_enabled=false
login_success=4 login_errors=0
send_success=2 send_errors=0
ack_success=2 ack_errors=0
status=PASS
```

- [ ] **Step 6: Run CTest**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected:

```text
100% tests passed
```

- [ ] **Step 7: Commit verification fixes if any were needed**

If Step 1 through Step 6 required code changes, run:

```bash
git add tests/business_stress_test.cc CMakeLists.txt config/stress.real.example.json
git commit -m "test: stabilize real backend stress test"
```

If no changes were needed, do not create an empty commit.

## Self-Review Notes

- Spec coverage: covered backend switching, optional Redis, run-scoped Redis prefix, phase-separated login and message metrics, prepare via `UserService`, duplicate run ID failure through `USER_ALREADY_EXISTS`, explicit server and Redis stream shutdown, success criteria including login count, CMake linkage, example config, and verification commands.
- Scope check: this plan implements only online one-to-one chat stress for memory and real backends. Offline pull, hot receiver, slow receiver, and cross-server scenarios stay outside this plan.
- Type consistency: plan uses existing `chat::ServerConfig`, `chat::DbPool`, `chat::RedisPool`, `chat::RedisClient`, `chat::RedisSessionStore`, `chat::RedisRateLimiter`, `chat::MessageDedupCache`, `chat::RedisPushStream`, `chat::UserRepository`, `chat::MessageRepository`, `chat::UserService`, `chat::ChatService`, `chat::MessageHandler`, and `TcpServer` interfaces.
