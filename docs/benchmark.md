# Benchmark 记录

## 业务压力测试

本次记录覆盖 `business_stress_test` 的一次在线单聊业务链路压测。压测目标是验证真实 TCP 连接、换行 JSON 分包、消息处理器、登录态绑定、在线消息推送和消息 ACK 是否能在并发客户端下保持业务正确性。

真实 MySQL 后端的业务压力测试和 in-memory benchmark 对比见 [Real Backend 业务压力测试报告](real_backend_stress_report.md)。

测试命令：

```bash
./build/business_stress_test --pairs=100 --messages-per-pair=20 --client-workers=8
```

测试规模：

- `pairs=100`
- `messages_per_pair=20`
- `client_workers=8`
- 预期登录成功数：`200`
- 预期发送消息数：`2000`
- 预期在线推送数：`2000`
- 预期 ACK 数：`2000`

## 测试数据

### 基线数据

基线版本未对 TCP 连接启用 `TCP_NODELAY`。

| 指标 | 数值 |
| --- | ---: |
| `login_success` | 200 |
| `login_errors` | 0 |
| `send_success` | 2000 |
| `send_errors` | 0 |
| `push_received` | 2000 |
| `lost_push` | 0 |
| `duplicate_push` | 0 |
| `push_validation_errors` | 0 |
| `ack_success` | 2000 |
| `ack_errors` | 0 |
| `connect_errors` | 0 |
| `protocol_errors` | 0 |
| `server_errors` | 0 |
| `login_p50_ms` | 0.33 |
| `login_p95_ms` | 0.59 |
| `send_p50_ms` | 0.52 |
| `send_p95_ms` | 0.79 |
| `push_p50_ms` | 40.70 |
| `push_p95_ms` | 41.69 |
| `ack_p50_ms` | 0.51 |
| `ack_p95_ms` | 0.87 |
| `total_duration_ms` | 10273.10 |
| `messages_per_second` | 194.68 |
| `status` | PASS |

### TCP_NODELAY 后数据

服务端 accepted socket 和压测客户端 socket 都启用 `TCP_NODELAY` 后，使用相同命令重新运行。

| 指标 | 数值 |
| --- | ---: |
| `login_success` | 200 |
| `login_errors` | 0 |
| `send_success` | 2000 |
| `send_errors` | 0 |
| `push_received` | 2000 |
| `lost_push` | 0 |
| `duplicate_push` | 0 |
| `push_validation_errors` | 0 |
| `ack_success` | 2000 |
| `ack_errors` | 0 |
| `connect_errors` | 0 |
| `protocol_errors` | 0 |
| `server_errors` | 0 |
| `login_p50_ms` | 0.22 |
| `login_p95_ms` | 0.81 |
| `send_p50_ms` | 0.26 |
| `send_p95_ms` | 0.42 |
| `push_p50_ms` | 0.28 |
| `push_p95_ms` | 0.45 |
| `ack_p50_ms` | 0.18 |
| `ack_p95_ms` | 0.30 |
| `total_duration_ms` | 144.92 |
| `messages_per_second` | 13800.48 |
| `status` | PASS |

### 当前 in-memory baseline

服务端 accepted socket 和压测客户端 socket 默认启用 `TCP_NODELAY`，服务端 listen backlog 调整为 `SOMAXCONN` 后，使用相同命令重新运行。本组数据作为当前 in-memory baseline。

| 指标 | 数值 |
| --- | ---: |
| `login_success` | 200 |
| `login_errors` | 0 |
| `send_success` | 2000 |
| `send_errors` | 0 |
| `push_received` | 2000 |
| `lost_push` | 0 |
| `duplicate_push` | 0 |
| `push_validation_errors` | 0 |
| `ack_success` | 2000 |
| `ack_errors` | 0 |
| `connect_errors` | 0 |
| `protocol_errors` | 0 |
| `server_errors` | 0 |
| `login_p50_ms` | 0.21 |
| `login_p95_ms` | 0.34 |
| `send_p50_ms` | 0.23 |
| `send_p95_ms` | 0.36 |
| `push_p50_ms` | 0.25 |
| `push_p95_ms` | 0.38 |
| `ack_p50_ms` | 0.16 |
| `ack_p95_ms` | 0.26 |
| `total_duration_ms` | 125.79 |
| `messages_per_second` | 15899.37 |
| `status` | PASS |

### 对比摘要

| 指标 | 基线 | TCP_NODELAY 后 | 变化 |
| --- | ---: | ---: | ---: |
| `push_p50_ms` | 40.70 | 0.28 | 降低约 99.31% |
| `push_p95_ms` | 41.69 | 0.45 | 降低约 98.92% |
| `messages_per_second` | 194.68 | 13800.48 | 提升约 70.89 倍 |
| `total_duration_ms` | 10273.10 | 144.92 | 降低约 98.59% |

## 发现的问题

基线压测中，`send_p50_ms` 和 `ack_p50_ms` 都低于 1ms，但 `push_p50_ms` 和 `push_p95_ms` 稳定在 40ms 左右，并且二者非常接近。

这说明问题不像业务处理或内存仓储导致的随机抖动，更像网络层存在固定等待窗口。由于当前协议是长连接上的小 JSON 包，且服务端和客户端之前都没有禁用 Nagle 算法，`message_push` 很容易受到 Nagle 算法和 delayed ACK 组合影响，表现为稳定的约 40ms 延迟。

本次重新压测时还遇到过一次启动阶段连接超时：`login_success=198`、`connect_errors=1`，错误为 `alice_3: connect timed out`。业务消息链路没有丢 push，但这组数据不能作为干净 baseline。继续排查后发现服务端 `listen(listen_fd_, 5)` 的 backlog 太小，高并发连接启动时 TCP 握手完成不等于应用层已 accept，监听队列可能短暂打满，导致压测客户端在 1 秒连接超时内失败。

## 排查过程

### 复现现象

先用较大压测规模复现用户观察到的数据：

```bash
./build/business_stress_test --pairs=100 --messages-per-pair=20 --client-workers=8
```

复现结果中，业务正确性全部通过，但 `push_p50_ms=40.70`、`push_p95_ms=41.69`，明显高于 `send` 和 `ack` 延迟。

### 缩小问题范围

观察同一份报告中的指标：

- `send_p50_ms=0.52`
- `send_p95_ms=0.79`
- `ack_p50_ms=0.51`
- `ack_p95_ms=0.87`
- `push_p50_ms=40.70`
- `push_p95_ms=41.69`

发送响应和 ACK 响应都很快，说明服务端业务处理、内存仓储、响应编码和普通请求响应路径没有明显 40ms 等待。只有跨连接的在线 push 路径出现稳定延迟，因此重点转向 TCP 小包发送行为。

### 提出假设

假设：`message_push` 的 40ms 级别延迟主要来自 TCP 小包延迟，即 Nagle 算法和 delayed ACK 的组合，而不是业务逻辑瓶颈。

判断依据：

- `push_p50_ms` 和 `push_p95_ms` 都集中在 40ms 左右。
- 40ms 是 Linux delayed ACK 场景中常见的量级。
- `send` 和 `ack` 延迟远低于 1ms，业务处理本身不慢。
- 当前协议包较小，且是请求响应和异步推送混合的长连接模型。

### 单变量验证

只改变一个变量：给服务端 accepted socket 和压测客户端 socket 都启用 `TCP_NODELAY`，然后用同一条压测命令复跑。

涉及位置：

```text
src/TcpServer.cc
tests/business_stress_test.cc
```

启用后，`push_p50_ms` 从 `40.70` 降到 `0.28`，`push_p95_ms` 从 `41.69` 降到 `0.45`，吞吐从 `194.68 msg/s` 提升到 `13800.48 msg/s`。这验证了 40ms push 延迟主要来自 TCP 小包等待。

### 连接超时排查

在 `TCP_NODELAY` 默认开启后，首次复跑同一压测命令出现一次 `connect_errors=1`。日志中第一批连接后出现约 1 秒空档，随后后续连接继续成功，说明问题集中在连接建立阶段，而不是登录、发送、推送或 ACK 业务处理阶段。

检查服务端启动路径发现 `startListen()` 使用 `listen(listen_fd_, 5)`。压测中即使客户端连接步骤做了互斥，`connect()` 成功也只代表连接进入内核队列，并不代表服务端应用层已经 accept。backlog 过小会让突发连接更容易碰到连接超时。将 backlog 调整为 `SOMAXCONN` 后，用完全相同命令复跑通过，`connect_errors=0`。

## 处理结果

本次验证后保留了 `TCP_NODELAY` 改动：

- 服务端在 accept 新连接后，对客户端 fd 设置 `TCP_NODELAY`。
- 压测客户端连接成功后，对本地 socket 设置 `TCP_NODELAY`。
- 服务端 listen backlog 从 `5` 调整为 `SOMAXCONN`，避免压测启动阶段突发连接打满过小监听队列。

这样更符合聊天长连接场景：在线消息、ACK、心跳等都是小包，业务更看重低延迟而不是通过 Nagle 合并小包来节省报文数量。

## 验证命令

构建：

```bash
cmake --build build
```

默认测试：

```bash
ctest --test-dir build --output-on-failure
```

业务压力测试：

```bash
./build/business_stress_test --pairs=100 --messages-per-pair=20 --client-workers=8
```

验证结果：

```text
100% tests passed, 0 tests failed out of 45
business_stress_test status=PASS
```

## 后续建议

- 将服务端连接参数配置化，例如增加 `tcp_nodelay` 配置项，默认开启。
- 在后续 benchmark 中固定记录机器信息、CPU、内核版本和构建类型，便于多次结果横向比较。
- 为压力测试增加 `--quiet` 或日志级别控制，避免大量连接日志影响报告可读性。
