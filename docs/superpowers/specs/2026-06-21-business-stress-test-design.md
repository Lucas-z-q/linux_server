# Business Stress Test Design

## Overview

Add a manually-run business stress test target for the online one-to-one chat path. The test should exercise the real TCP server, packet framing, JSON protocol, message handler, user service, chat service, session binding, online push, and message acknowledgement flow while replacing MySQL and Redis with in-memory test dependencies.

The first version is a developer-machine quick stress test. It should be easy to build and run locally, should not depend on external services, and should not run as part of the default `ctest` suite.

## Goals

- Cover the online chat business path through real TCP connections.
- Verify login, `send_message`, `message_push`, and `message_ack` correctness under concurrent client load.
- Avoid MySQL, Redis, and bcrypt noise so failures are easier to localize.
- Produce a compact report with success counts, error counts, latency percentiles, total duration, and throughput.
- Return a non-zero exit code when business correctness fails.

## Non Goals

- Do not stress real MySQL or Redis.
- Do not test offline message pull in the first version.
- Do not discover maximum capacity or enforce machine-dependent latency thresholds.
- Do not run this target from default `ctest`.
- Do not test bcrypt performance.

## Selected Approach

Create a new executable target named `business_stress_test`, with the main source file at `tests/business_stress_test.cc`.

The executable starts a real `TcpServer` in-process on a background thread and then starts client worker threads in the same process. The server uses real production components for network and business orchestration:

```text
TcpServer
  -> PacketCodec / ConnectionContext
  -> MessageHandler / JsonCodec
  -> UserService / ChatService / SessionManager
  -> in-memory repositories and session store
```

The CMake target should be built by the normal build, but it should not be registered with `add_test`. This keeps the binary available for manual use while avoiding slow or noisy default test runs.

## Default Run

The default run should be conservative:

```bash
./build/business_stress_test
```

Default options:

```text
--pairs=50
--messages-per-pair=10
--client-workers=4
--port=0
--connect-timeout-ms=1000
--request-timeout-ms=3000
--push-timeout-ms=3000
--content-size=32
--verbose=false
```

This produces 100 TCP client connections, 100 login requests, 500 sent messages, 500 expected online pushes, and 500 acknowledgements. Passing `--port=0` lets the OS choose an available port; the client side reads the actual port from `TcpServer::getPort()`.

The option name `--client-workers` is intentional. It must refer only to client-side load generation threads. The current server worker pool remains the existing fixed `TcpServer` worker pool.

## Test Data

Each pair gets unique users to avoid repeated-login token revocation and sender multi-device sync noise:

```text
alice_0 -> bob_0
alice_1 -> bob_1
...
alice_N -> bob_N
```

The in-memory user repository should precreate `2 * pairs` users. Each user uses a fast fake password hash, not bcrypt.

## Components

### StressOptions

Parse command-line options and validate that counts and timeouts are positive. Invalid options should print usage and exit non-zero before starting the server.

### FastPasswordHasher

Implement `chat::IPasswordHasher` with deterministic fast behavior:

- `Hash(password)` returns a stable fake encoded hash.
- `Verify(password, encoded_hash)` validates the fake encoded hash.
- `NeedsRehash(encoded_hash)` returns `false`.

This avoids turning the chat stress test into a bcrypt benchmark.

### StressUserRepository

Implement `chat::IUserRepository` for the precreated test users. It should support:

- `findByUsername`
- `findById`
- `createUser`
- `updatePasswordHash`

The implementation should be thread-safe. A coarse `std::mutex` is acceptable because this repository is test infrastructure, not the subject under stress.

### StressMessageRepository

Implement `chat::IMessageRepository` for the stress test. Do not reuse the current `FakeMessageRepository` directly because it is not thread-safe.

The stress repository should model the chat semantics that the service depends on:

- Stable single-chat `conversation_id` for each unordered user pair.
- Per-conversation monotonic `sequence`.
- Message id storage.
- `(from_user_id, client_msg_id)` idempotency.
- Conflict preservation when the same client message id maps to different content.
- Offline message storage behavior for future compatibility.
- `markDelivered` with ownership and state checks.
- `markRead` with valid state progression.

A coarse mutex around the repository state is acceptable for the first version.

### StressClientConnection

Wrap one TCP client socket and provide:

- Connect with timeout.
- Send one JSON request using `PacketCodec`.
- Continuously read newline-delimited JSON packets.
- Route `seq > 0` packets to pending responses.
- Route `message_push` packets to a push queue.
- Count or ignore `message_sync_push` packets explicitly.
- Report protocol, timeout, or socket errors with enough context for debugging.

The read side must not assume a strict one-request-one-response stream. Bob can receive `message_push` before or after other responses, and future scenarios may add more asynchronous messages.

### StressMetrics

Track counters and successful latency samples:

- Login successes and errors.
- Send successes and errors.
- Pushes received, lost pushes, duplicate pushes, and push validation errors.
- Ack successes and errors.
- Protocol errors.
- Connect errors.
- Server readiness errors.
- `login_p50_ms`, `login_p95_ms`.
- `send_p50_ms`, `send_p95_ms`.
- `push_p50_ms`, `push_p95_ms`.
- `ack_p50_ms`, `ack_p95_ms`.
- `total_duration_ms`.
- `messages_per_second`.

Only successful operations should contribute latency samples. Failed operations should be counted separately and summarized with the first several error messages.

## Data Flow

The main function runs these steps:

1. Parse and validate `StressOptions`.
2. Precreate test users and construct in-memory repositories, session store, fast hasher, `SessionManager`, services, handler, and `TcpServer`.
3. Start `TcpServer::start()` on a background thread.
4. Wait until the server is ready. With `--port=0`, wait until `server.getPort()` becomes non-zero, then perform connection retries against that port.
5. Split pair indices across `--client-workers`.
6. For each pair, open Alice and Bob TCP connections.
7. Log Alice and Bob in on their own connections.
8. For each message in the pair:
   - Alice sends `send_message`.
   - Alice waits for `send_message_resp` and validates `code`, `seq`, `message_id`, `conversation_id`, and `to_user_id`.
   - Bob waits for the matching `message_push` and validates `message_id`, `conversation_id`, `from_user_id`, `to_user_id`, and `content`.
   - Bob sends `message_ack` for the pushed `message_id`.
   - Bob waits for `message_ack_resp` and validates success.
9. Join all client workers.
10. Stop the server and join the server thread.
11. Print the final report.
12. Return `0` only when correctness criteria pass.

## Success Criteria

The run passes only if all of these are true:

```text
send_success == pairs * messages_per_pair
push_received == pairs * messages_per_pair
ack_success == pairs * messages_per_pair
lost_push == 0
duplicate_push == 0
error_count == 0
```

No hard latency or throughput threshold is required in the first version. Latency and throughput are reported as a baseline for future comparison.

## Error Categories

The report should include these error categories:

- `connect_errors`: socket creation, connect, or timeout failures.
- `login_errors`: login response not OK, seq mismatch, or missing token.
- `send_errors`: send response not OK, seq mismatch, missing fields, or empty message id.
- `push_errors`: missing push, duplicate push, or push field mismatch.
- `ack_errors`: ack response not OK, seq mismatch, or invalid affected rows.
- `protocol_errors`: JSON parse failure, unknown message type, or response routed to no waiter.
- `server_errors`: server startup, readiness, or shutdown failures.

## CMake Integration

Add a normal executable target:

```cmake
add_executable(business_stress_test
    tests/business_stress_test.cc
    src/TcpServer.cc
    src/TcpConnection.cc
    src/ConnectionContext.cc
    src/codec/json_codec.cc
    src/codec/packet_codec.cc
    src/handler/message_handler.cc
    src/server/session_manager.cc
    src/service/user_service.cc
    src/service/chat_service.cc
    src/service/friend_service.cc
    src/service/group_service.cc
    src/concurrency/thread_pool.cc
)

target_include_directories(business_stress_test PRIVATE include third_party tests)
target_link_libraries(business_stress_test PRIVATE chat_logger security_runtime Threads::Threads)
```

Do not call `add_test` for this target. Do not use `EXCLUDE_FROM_ALL` in the first version.

## Verification

After implementation, verify with:

```bash
cmake --build build
./build/business_stress_test
```

The test should print a compact report and return `0` on success. Any correctness failure should return non-zero.

## Future Extensions

Future scenarios can extend the same harness with explicit scenario flags:

```text
--scenario=online_chat
--scenario=offline_pull
--scenario=hot_receiver
--scenario=slow_receiver
```

Those scenarios are intentionally out of scope for the first version.
