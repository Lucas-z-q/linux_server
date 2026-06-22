# Real Backend 业务压力测试报告

## 测试目标

本报告记录 `business_stress_test` 在真实 MySQL 后端下的业务压力测试结果，并与现有 `docs/benchmark.md` 中的 in-memory benchmark 做对比。

压测覆盖的业务链路为：

```text
客户端登录 -> 在线单聊发送 -> 接收方收到 message_push -> 接收方 message_ack
```

本次测试关注四点：

- 真实 MySQL 后端下，发送、在线推送和 ACK 是否保持业务正确性。
- 与 in-memory benchmark 相比，真实持久化路径带来的吞吐和延迟差异。
- 不同 `client_workers` 下，吞吐平台和 p95 延迟拐点在哪里。
- 长时间 soak 下，区分后端稳定性问题和压测器调度方式带来的空闲超时。

## 测试环境和配置

测试程序：

```text
./build/business_stress_test
```

真实后端配置：

```text
backend=real
config=config/stress.real.local.json
redis_enabled=false
password_hasher=fast
```

说明：

- `real` 后端使用真实 MySQL 连接池、真实用户仓储和真实消息仓储。
- Redis 未启用，因此本次不覆盖 Redis session、缓存、限流和跨服推送。
- `password_hasher=fast` 不包含生产 bcrypt 成本，因此登录延迟不能代表生产密码哈希开销。
- `config/stress.real.local.json` 为本地配置文件，包含本机 MySQL 账号信息，不应提交到 git。

## 真实后端长期基线

测试命令：

```bash
./build/business_stress_test \
  --backend=real \
  --config=config/stress.real.local.json \
  --pairs=100 \
  --messages-per-pair=100 \
  --client-workers=4
```

测试规模：

- `pairs=100`
- `messages_per_pair=100`
- `client_workers=4`
- 预期登录成功数：`200`
- 预期发送消息数：`10000`
- 预期在线推送数：`10000`
- 预期 ACK 数：`10000`

测试结果：

| 指标 | 数值 |
| --- | ---: |
| `login_success` | 200 |
| `login_errors` | 0 |
| `send_success` | 10000 |
| `send_errors` | 0 |
| `push_received` | 10000 |
| `lost_push` | 0 |
| `duplicate_push` | 0 |
| `push_validation_errors` | 0 |
| `ack_success` | 10000 |
| `ack_errors` | 0 |
| `connect_errors` | 0 |
| `protocol_errors` | 0 |
| `server_errors` | 0 |
| `login_p50_ms` | 0.51 |
| `login_p95_ms` | 0.78 |
| `send_p50_ms` | 8.42 |
| `send_p95_ms` | 10.88 |
| `push_p50_ms` | 8.44 |
| `push_p95_ms` | 10.90 |
| `ack_p50_ms` | 7.08 |
| `ack_p95_ms` | 10.44 |
| `login_duration_ms` | 88.62 |
| `message_duration_ms` | 39550.79 |
| `messages_per_second` | 252.84 |
| `status` | PASS |

吞吐计算：

```text
10000 / 39.55079s = 252.84 msg/s
```

本组结果说明，在 200 个在线客户端、10000 条正式消息规模下，真实 MySQL 后端业务链路完整通过。测试期间没有出现发送失败、push 丢失、重复 push、push 内容错误或 ACK 失败。

`send` 和 `push` 延迟几乎一致：

```text
send_p95_ms=10.88
push_p95_ms=10.90
```

这说明在线 push 路径没有出现额外明显排队。当前主要耗时更可能集中在消息发送处理、MySQL 写入和 ACK 更新路径。

## 长短测试稳定性对比

同样使用 `pairs=100` 和 `client_workers=4`，分别测试 2000 条和 10000 条正式消息：

| 指标 | 2000 条消息 | 10000 条消息 | 变化 |
| --- | ---: | ---: | --- |
| `messages_per_second` | 251.60 | 252.84 | 基本持平 |
| `send_p50_ms` | 8.48 | 8.42 | 基本持平 |
| `send_p95_ms` | 10.90 | 10.88 | 基本持平 |
| `push_p50_ms` | 8.50 | 8.44 | 基本持平 |
| `push_p95_ms` | 10.97 | 10.90 | 基本持平 |
| `ack_p50_ms` | 7.19 | 7.08 | 基本持平 |
| `ack_p95_ms` | 10.06 | 10.44 | 小幅上升 |

放大到 10000 条消息后，吞吐和主要延迟指标没有明显劣化，说明当前规模下没有观察到持续排队、连接泄漏或越跑越慢的问题。

## 客户端并发对比

在 `pairs=100`、`messages_per_pair=20` 下，对比 `client_workers=4` 和 `client_workers=8`：

| 指标 | `client_workers=4` | `client_workers=8` | 观察 |
| --- | ---: | ---: | --- |
| `messages_per_second` | 251.60 | 260.09 | 仅提升约 3.37% |
| `login_p95_ms` | 0.88 | 14.32 | 明显变差 |
| `send_p95_ms` | 10.90 | 20.22 | 接近翻倍 |
| `push_p95_ms` | 10.97 | 20.24 | 接近翻倍 |
| `ack_p95_ms` | 10.06 | 19.49 | 接近翻倍 |
| `status` | PASS | PASS | 正确性均通过 |

提高客户端 worker 数后，吞吐只小幅提升，但尾延迟明显上升。当前配置下，`client_workers=4` 是更稳定的压测并发点；`client_workers=8` 更像是在增加服务端或 MySQL 路径上的排队压力。

## 客户端 worker 阶梯测试

为了寻找真实后端的吞吐平台，对 `client_workers=4,8,16,32,64` 做阶梯测试。测试使用真实 MySQL 后端，MySQL 连接池采用连接池压测后推荐的低并发配置。

测试命令模板：

```bash
for workers in 4 8 16 32 64; do
  run_id="workers_${workers}_$(date +%Y%m%d%H%M%S)"

  env -u CHAT_DB_HOST -u CHAT_DB_PORT -u CHAT_DB_USER -u CHAT_DB_PASSWORD -u CHAT_DB_NAME \
    ./build/business_stress_test \
    --backend=real \
    --config=config/stress.real.local.json \
    --run-id="$run_id" \
    --pairs=200 \
    --messages-per-pair=20 \
    --warmup-messages=1 \
    --client-workers="$workers" \
    | tee /tmp/stress-workers-${workers}.log
done
```

单轮结果：

| workers | status | msg/s | send p95 ms | ack p95 ms | message duration ms |
| ---: | --- | ---: | ---: | ---: | ---: |
| 4 | PASS | 245.70 | 11.07 | 10.42 | 16280.26 |
| 8 | PASS | 252.10 | 20.90 | 19.82 | 15866.64 |
| 16 | PASS | 259.79 | 38.19 | 37.41 | 15397.14 |
| 32 | PASS | 264.34 | 73.01 | 71.27 | 15132.02 |
| 64 | PASS | 262.40 | 141.85 | 142.82 | 15243.91 |

随后对 `client_workers=16,32,64` 做三轮复测：

| workers | avg msg/s | avg send p95 ms | avg ack p95 ms | rounds |
| ---: | ---: | ---: | ---: | ---: |
| 16 | 257.99 | 38.44 | 37.81 | 3 |
| 32 | 261.95 | 74.16 | 73.02 | 3 |
| 64 | 268.05 | 141.32 | 140.55 | 3 |

复测后更准确的结论是：吞吐平台从 `client_workers=32` 附近开始。`client_workers=64` 仍可榨出约 `2.3%` 的平均吞吐提升，但主要代价是排队延迟，`send_p95_ms` 从 `74.16` 上升到 `141.32`，接近翻倍。因此 `client_workers=64` 更适合作为过载观察点，不适合作为常规 benchmark 基线。

本轮 MySQL 状态观测里，短压测几乎没有行锁等待增量：

```text
Innodb_row_lock_waits_delta: mostly 0, max 2
Innodb_row_lock_time_delta_ms: max 11
Threads_connected: 1 -> 1
Threads_running: 2 -> 2
Max_used_connections: 32 -> 32
```

这说明 worker 阶梯里的尾延迟上升主要来自应用侧、压测调度或数据库正常排队成本，没有证据表明出现明显 MySQL 行锁等待爆炸。

## Soak 长跑稳定性确认

第一次 soak 使用如下规模：

```text
pairs=200
messages_per_pair=200
client_workers=4
heartbeat_timeout_ms=90000
expected_messages=40000
```

该轮结果为 FAIL：

| 指标 | 数值 |
| --- | ---: |
| `send_success` | 23200 |
| `send_errors` | 84 |
| `ack_success` | 23200 |
| `send_p95_ms` | 10.96 |
| `ack_p95_ms` | 10.36 |
| `message_duration_ms` | 93506.02 |
| `effective_messages_per_second` | 248.11 |

失败表现为 `response timed out`，首批错误从 `pair 112` 开始。服务端 stderr 中出现大量连接在约 `90s` 空闲后因 `heartbeat_timeout` 关闭。MySQL 观测为：

```text
row_lock_waits_delta=26
row_lock_time_delta_ms=111
```

这组 MySQL 数据不支持“明显 MySQL 行锁等待爆炸”。结合压测程序当前按 pair 分配消息的实现，可以判断该失败不是后端长期稳定性失败，而是当前 `pair-at-a-time` 调度方式导致大量已登录连接长期空闲：`pairs=200`、`client_workers=4`、`messages_per_pair=200` 时，压测器会让每个 worker 先跑完一个 pair 的全部消息，再处理下一个 pair。后面的 pair 已登录但长时间没有业务收发，超过 `90s` heartbeat 阈值后被服务端正常关闭。

随后做了两组确认测试。

第一组保持失败参数不变，只将 heartbeat timeout 提高到 `300s`。由于配置校验要求 `redis.presence_ttl_seconds` 不小于 heartbeat 秒数，临时测试配置同时将 `presence_ttl_seconds` 提高到 `600`：

| config | result | msg/s | send p95 ms | ack p95 ms | errors |
| --- | --- | ---: | ---: | ---: | ---: |
| `pairs=200 messages=200 workers=4 heartbeat=300s` | PASS | 245.45 | 11.09 | 10.42 | 0 |

第二组保持原始 `90s` heartbeat，但改成真正低并发长跑，让所有 pair 一开始都参与发送：

| config | result | msg/s | send p95 ms | ack p95 ms | errors |
| --- | --- | ---: | ---: | ---: | ---: |
| `pairs=4 messages=10000 workers=4 heartbeat=90s` | PASS | 245.58 | 11.18 | 10.41 | 0 |

两组确认测试的 stderr 均没有匹配到 `heartbeat_timeout`、`response timed out` 或 error 类记录。MySQL 行锁增量仍然很小：

| config | row lock waits delta | row lock time delta ms |
| --- | ---: | ---: |
| `heartbeat=300s` | 40 | 159 |
| `pairs=4 workers=4` | 21 | 81 |

因此，本轮 soak 的最终结论是：真实活跃低并发长跑 40000 条消息通过，吞吐约 `245 msg/s`，`send_p95_ms` 约 `11ms`，`ack_p95_ms` 约 `10ms`。此前 `pairs=200 messages=200 workers=4 heartbeat=90s` 的 FAIL 应记录为压测器调度限制，而不是后端长期稳定性失败。

## 与 in-memory benchmark 对比

`docs/benchmark.md` 中当前 in-memory baseline 使用如下命令：

```bash
./build/business_stress_test --pairs=100 --messages-per-pair=20 --client-workers=8
```

该 baseline 使用 in-memory 仓储，服务端 accepted socket 和压测客户端 socket 默认启用 `TCP_NODELAY`，服务端 listen backlog 使用 `SOMAXCONN`。

为了做同规模对比，真实后端选用以下结果：

```text
backend=real
pairs=100
messages_per_pair=20
client_workers=8
expected_messages=2000
```

对比结果：

| 指标 | in-memory baseline | real MySQL | 观察 |
| --- | ---: | ---: | --- |
| `send_success` | 2000 | 2000 | 均全部成功 |
| `push_received` | 2000 | 2000 | 均全部送达 |
| `lost_push` | 0 | 0 | 均无丢 push |
| `duplicate_push` | 0 | 0 | 均无重复 push |
| `ack_success` | 2000 | 2000 | 均全部 ACK |
| `send_p50_ms` | 0.23 | 15.21 | real 约 66.13 倍 |
| `send_p95_ms` | 0.36 | 20.22 | real 约 56.17 倍 |
| `push_p50_ms` | 0.25 | 15.23 | real 约 60.92 倍 |
| `push_p95_ms` | 0.38 | 20.24 | real 约 53.26 倍 |
| `ack_p50_ms` | 0.16 | 14.39 | real 约 89.94 倍 |
| `ack_p95_ms` | 0.26 | 19.49 | real 约 74.96 倍 |
| `messages_per_second` | 15899.37 | 260.09 | in-memory 约 61.13 倍 |
| `status` | PASS | PASS | 均通过 |

两组测试的业务正确性均通过。差异主要体现在延迟和吞吐：in-memory benchmark 不经过真实数据库持久化，主要验证协议、连接、会话和在线推送路径；real MySQL 测试覆盖真实用户和消息持久化，因此吞吐显著下降、发送和 ACK 延迟明显升高。

从 `send` 和 `push` 的关系看：

```text
in-memory: send_p95_ms=0.36, push_p95_ms=0.38
real MySQL: send_p95_ms=20.22, push_p95_ms=20.24
```

两种后端下 `send` 和 `push` 都几乎重合。这说明 `TCP_NODELAY` 和 backlog 优化后，在线 push 路径本身没有成为主要瓶颈。真实后端的主要成本来自 MySQL 写入、查询和 ACK 更新等持久化路径。

## 结论

真实 MySQL 后端在以下长期基线下测试通过：

```text
100 对用户
200 个在线客户端
10000 条正式消息
4 个客户端 worker
```

长期基线结果：

```text
throughput ~= 252.84 msg/s
send_p95_ms ~= 10.88
push_p95_ms ~= 10.90
ack_p95_ms ~= 10.44
errors = 0
status = PASS
```

与 in-memory benchmark 相比，真实 MySQL 后端吞吐低约 60 倍量级，消息发送和 ACK 的尾延迟高一个到两个数量级。这符合预期，因为真实后端覆盖了数据库连接池、用户表、消息表、消息状态更新和相关索引维护成本。

当前更合理的真实后端压测并发点是 `client_workers=4`。继续增加到 `client_workers=8` 时，吞吐提升很小，但 p95 延迟明显变差。

后续 worker 阶梯和 soak 复测进一步细化了这个结论：

- `client_workers=4` 是低延迟基线，真实活跃长跑 40000 条消息通过。
- `client_workers=32` 是更合理的饱和观察点，吞吐已经接近平台，p95 尚未达到 `client_workers=64` 的过载水平。
- `client_workers=64` 只带来约 `2.3%` 平均吞吐提升，但 p95 接近翻倍，应作为过载和排队验证点。
- `pairs=200 messages=200 workers=4 heartbeat=90s` 的 soak FAIL 来自压测器 `pair-at-a-time` 调度造成的连接空闲超时，不应视为后端长期稳定性失败。

## 后续建议

- 将 `client_workers=4`、`pairs=4`、`messages_per_pair=10000` 作为当前 real MySQL 低延迟长跑基线。
- 将 `client_workers=32`、`pairs=200`、`messages_per_pair=20` 作为当前 real MySQL 饱和观察点。
- 将 `client_workers=64` 作为过载观察点，只用于观察排队和尾延迟恶化。
- 将 MySQL 连接池默认压测配置暂定为 `min_connections=4`、`max_connections=4`。
- 给 `business_stress_test` 增加 round-robin message scheduler，避免 `pair-at-a-time` 模式在 `pairs >> workers` 且 `messages_per_pair` 很大时制造大量空闲连接。
- 在 FAIL 报告中增加 `effective_messages_per_second = send_success / duration`，避免失败时用预期消息数计算吞吐造成误导。
- 后续优化优先关注 MySQL 写路径、ACK 更新路径、事务和索引成本。
- 在启用 Redis 后补充一组 `redis_enabled=true` 的同规模对比，区分缓存、限流、session store 和跨服推送组件带来的影响。
- 后续 benchmark 固定记录 MySQL 版本、连接池大小、CPU、磁盘和构建类型，便于跨机器和跨提交比较。
