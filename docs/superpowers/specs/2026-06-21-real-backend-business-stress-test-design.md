# Real Backend Business Stress Test Design

## Overview

Extend the existing manually-run `business_stress_test` so it can run the same online one-to-one chat stress scenario against either in-memory test dependencies or real MySQL/optional Redis dependencies.

This is a real-dependency integration stress test. It does not replace the current in-memory business stress test. The in-memory backend remains the fast baseline for the TCP, packet codec, handler, service, session, push, and ack business loop. The real backend adds visibility into MySQL persistence cost, optional Redis session/cache/rate-limit/dedup/push-stream cost, connection pool sizing, and p50/p95 latency under realistic dependencies.

## Goals

- Reuse the existing client stress model for fair `memory` versus `real` comparison.
- Add `--backend=memory|real` and `--config=...` to `business_stress_test`.
- Use real MySQL repositories in `real` backend.
- Create Redis runtime components only when `redis.enabled=true`.
- Keep `--port` as the stress harness listen-port override, defaulting to `0`.
- Keep user/database setup outside timed message measurement.
- Print enough dependency and port information to interpret results.
- Keep the target manually run and outside default `ctest`.

## Non Goals

- Do not create, migrate, or drop MySQL databases from the stress binary in the first version.
- Do not submit real database credentials or local config files.
- Do not make Redis mandatory for `real` backend.
- Do not enforce machine-dependent latency or throughput thresholds.
- Do not add offline pull, hot receiver, slow receiver, or cross-server scenarios in this version.
- Do not replace the in-memory stress repository path.

## Selected Approach

Use one binary and one client stress implementation:

```bash
./build/business_stress_test --backend=memory
./build/business_stress_test --backend=real --config=config/stress.real.json
```

The client side remains unchanged in shape:

```text
Alice TCP connection logs in
Bob TCP connection logs in
Alice sends send_message
Bob waits for message_push
Bob sends message_ack
```

Only server dependency construction changes by backend:

```text
memory:
  TcpServer
    -> MessageHandler
    -> UserService / ChatService
    -> StressUserRepository / StressMessageRepository
    -> local SessionManager

real:
  TcpServer
    -> MessageHandler
    -> UserService / ChatService
    -> UserRepository or CachedUserRepository
    -> MessageRepository
    -> DbPool / MySQL
    -> optional RedisSessionStore / RedisRateLimiter / MessageDedupCache / RedisPushStream
```

This approach gives the fairest comparison because both backends use the same client concurrency, request sequencing, push validation, ack validation, latency sampling, and report format.

## Command Surface

Existing options remain valid. New options:

```text
--backend=memory|real
--config=config/stress.real.json
--password-hasher=fast|bcrypt
--warmup-messages=N
--prepare-users=true|false
--run-id=VALUE
```

Defaults:

```text
backend=memory
config=
password_hasher=fast
warmup_messages=0
prepare_users=true
run_id=auto-generated
port=0
```

`--config` is required when `--backend=real`. It uses the existing `ServerConfig` JSON schema. The binary should reject unknown backend or password hasher values before starting the server.

`--run-id` is optional when `--prepare-users=true`. If `--prepare-users=false`, `--run-id` is required because the binary must know which existing users to log in as. `run_id` values should be limited to letters, digits, dashes, and underscores so generated usernames remain valid.

## Configuration

The first real-backend version assumes the database already exists and has been initialized with:

```bash
mysql -u root -p -e "CREATE DATABASE IF NOT EXISTS chat_stress CHARACTER SET utf8mb4;"
mysql -u root -p chat_stress < sql/001_create_chat_tables.sql
```

The repository may include a safe example file:

```text
config/stress.real.example.json
```

It should not include real credentials. Users can copy it locally or override sensitive fields with existing environment variables such as `CHAT_DB_PASSWORD`.

The stress harness ignores `config.server.listen_port` for listening. It uses `--port`, with `0` meaning the OS assigns an available port. The report must show both values:

```text
actual_port=43127
config_server_port_ignored=8080
```

This avoids changing production `ConfigLoader` validation, where `server.listen_port` remains `1..65535`.

## Architecture

Introduce a narrow lifecycle object for the stress server:

```cpp
struct StressServerBundle {
    std::unique_ptr<TcpServer> server;
    std::thread server_thread;
    int actual_port = 0;
    bool start_result = false;

    bool StartAndWaitReady(const StressOptions& options, std::string* error);
    void StopAndJoin();
};
```

The actual implementation should also hold dependencies whose lifetime must outlive `TcpServer`, including repositories, pools, services, handler, session manager, and optional Redis push stream. The important rule is explicit shutdown:

```text
StartAndWaitReady:
  start server on server_thread
  wait until server.getPort() is non-zero when --port=0
  validate that a client can connect

StopAndJoin:
  stop RedisPushStream if present
  call server.stop()
  join server_thread
  then allow dependencies to destruct in reverse ownership order
```

The server thread must never outlive the objects referenced by the handler, services, repositories, Redis stream callbacks, or pools.

## Backend Factories

Add backend construction functions:

```cpp
std::unique_ptr<StressServerBundle> CreateMemoryBackend(const StressOptions& options);

std::unique_ptr<StressServerBundle> CreateRealBackend(const StressOptions& options,
                                                       const chat::ServerConfig& config);
```

`CreateMemoryBackend` reuses the existing `StressUserRepository`, `StressMessageRepository`, `FastPasswordHasher`, local `SessionManager`, `UserService`, `ChatService`, and `MessageHandler`.

`CreateRealBackend` initializes:

- `DbPool` with `config.mysql` and `config.mysql_pool`.
- `UserRepository`.
- Optional `RedisPool` and `RedisClient` only when `config.redis.enabled=true`.
- Optional `CachedUserRepository`, `RedisSessionStore`, `RedisRateLimiter`, `MessageDedupCache`, and `RedisPushStream` only when Redis is enabled.
- `MessageRepository`.
- `SessionManager`.
- `UserService`, `ChatService`, and `MessageHandler`.
- `TcpServer` using `options.port`, not `config.server.listen_port`.

To reduce drift from production startup, implementation should extract a small shared runtime assembly helper from `src/main.cc` if it can be done cleanly. A narrow helper is preferred over a large application framework. If extraction would create excessive churn, the implementation plan may keep a local real-backend factory but must preserve the same dependency order and Redis conditions as `src/main.cc`.

## Redis Behavior

`real` backend does not imply Redis. Redis components are conditional:

```text
redis.enabled=false:
  use UserRepository directly
  use local SessionManager only
  no RedisPool, RedisClient, RedisSessionStore, RedisRateLimiter, MessageDedupCache, RedisPushStream

redis.enabled=true:
  initialize RedisPool
  wrap UserRepository with CachedUserRepository
  use RedisSessionStore for token/presence
  use RedisRateLimiter, MessageDedupCache, and RedisPushStream
```

If Redis is enabled and initialization fails, the run exits before prepare or measure and reports `redis_init_errors`.

## Password Hasher Modes

`real` backend password hashing is parameterized:

```text
--password-hasher=fast
  default
  prepare writes fast fake hashes
  login uses the matching FastPasswordHasher
  avoids bcrypt cost when measuring message path

--password-hasher=bcrypt
  prepare writes bcrypt hashes
  login uses production BcryptPasswordHasher
  useful when the run intentionally includes realistic login cost
```

The default is `fast` because the main purpose of this stress test is the message send, push, and ack path. Login latency is still measured and reported, but bcrypt should not be forced into every real-dependency message benchmark.

## Data Flow

The run has three phases.

### Prepare

Generate a unique `run_id`, or use the value from `--run-id`, then create users through the real repository path when `--backend=real` and `--prepare-users=true`.

Usernames:

```text
stress_<run_id>_alice_000001
stress_<run_id>_bob_000001
stress_<run_id>_alice_000002
stress_<run_id>_bob_000002
```

Client message IDs:

```text
<run_id>_<pair_id>_<message_index>
```

Prepare failures are reported as `prepare_errors` and stop the run before measurement. This prevents missing or partially prepared users from appearing as login or business-path failures.

When `--prepare-users=false`, the binary skips user creation and assumes the selected `run_id` users already exist with password hashes compatible with `--password-hasher`.

### Warmup

`--warmup-messages=N` sends optional messages before measurement. The default is `0`. Warmup results are validated but excluded from throughput and latency percentiles.

### Measure

Measure only:

```text
login
send_message
message_push
message_ack
```

Registration, schema creation, database cleanup, and Redis cleanup are outside the timed measure phase.

## Data Isolation

MySQL isolation in the first version is by unique `run_id` data. The binary does not delete MySQL rows. Users should run against a dedicated stress database such as `chat_stress`.

Redis isolation should use a dedicated Redis database or key prefix in config:

```text
redis.database=15
redis.key_prefix=stress
```

The report prints Redis database and key prefix when Redis is enabled. This makes accidental use of a development Redis namespace visible.

## Metrics and Report

Existing counters remain:

```text
login_success / login_errors
send_success / send_errors
push_received / lost_push / duplicate_push / push_validation_errors
ack_success / ack_errors
connect_errors / protocol_errors / server_errors
```

New or refined real-backend counters:

```text
config_errors
prepare_errors
mysql_init_errors
redis_init_errors
```

Report header:

```text
Business stress test report
  backend=real
  actual_port=43127 config_server_port_ignored=8080
  mysql=127.0.0.1:3306/chat_stress pool=4..16
  redis_enabled=true redis=127.0.0.1:6379/15 key_prefix=stress
  password_hasher=fast
  run_id=stress_20260621_...
  pairs=100 messages_per_pair=20 client_workers=8
  expected_messages=2000
```

Latency and throughput remain:

```text
login_p50_ms / login_p95_ms
send_p50_ms / send_p95_ms
push_p50_ms / push_p95_ms
ack_p50_ms / ack_p95_ms
total_duration_ms
messages_per_second
```

Only successful operations contribute latency samples. Failures are counted separately, and the first several error messages are printed.

## Success Criteria

A measured run passes only when:

```text
send_success == pairs * messages_per_pair
push_received == pairs * messages_per_pair
ack_success == pairs * messages_per_pair
lost_push == 0
duplicate_push == 0
error_count == 0
```

No latency or throughput threshold is required in this version.

## Error Handling

Initialization errors stop before measurement:

- `config_errors`: missing `--config`, invalid config, or invalid CLI option.
- `mysql_init_errors`: DbPool initialization failure.
- `redis_init_errors`: Redis initialization failure when Redis is enabled.
- `prepare_errors`: failed user creation or password hashing in prepare.

Runtime errors are counted in the existing categories:

- `server_errors`: server startup, readiness, or shutdown issues.
- `connect_errors`: client socket creation, connect, or timeout failures.
- `login_errors`: login response failure, seq mismatch, or missing token.
- `send_errors`: send response failure or invalid send response fields.
- `push_errors`: missing, duplicate, or invalid push.
- `ack_errors`: ack response failure or invalid ack response fields.
- `protocol_errors`: JSON parse failure or unmatched response.

The binary exits non-zero for any initialization error or failed success criteria.

## Build Integration

Keep `business_stress_test` as a normal build target, not a CTest target.

Real backend will need additional sources and libraries already used by the main server, including database, Redis, cache, stream, config, and app runtime sources. The implementation plan should update `CMakeLists.txt` without registering `add_test` for this target.

## Verification

Always verify the baseline:

```bash
cmake --build build
./build/business_stress_test --backend=memory
```

Verify real MySQL after preparing `chat_stress`:

```bash
./build/business_stress_test \
  --backend=real \
  --config=config/stress.real.json \
  --pairs=20 \
  --messages-per-pair=10 \
  --client-workers=4
```

Verify Redis-enabled real backend only when Redis is available and the config uses a dedicated database or key prefix:

```bash
./build/business_stress_test \
  --backend=real \
  --config=config/stress.real.redis.json \
  --pairs=20 \
  --messages-per-pair=10 \
  --client-workers=4
```

## Future Extensions

Future specs can extend the same harness with:

```text
--scenario=offline_pull
--scenario=hot_receiver
--scenario=slow_receiver
--scenario=cross_server_push
```

Those scenarios are intentionally out of scope for this design.
