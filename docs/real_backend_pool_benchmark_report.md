<!-- markdownlint-disable MD013 -->

# Real Backend MySQL 连接池压力测试报告

## 测试目标

本报告记录 `business_stress_test` 在真实 MySQL 后端下，对不同 MySQL 连接池大小的压力测试结果。测试目标是寻找当前业务链路在本地环境下的连接池拐点，并判断继续增大连接池是否带来稳定收益。

压测覆盖的业务链路为：

```text
客户端登录 -> 在线单聊发送 -> 接收方收到 message_push -> 接收方 message_ack
```

本次测试关注三点：

- 真实 MySQL 后端下，不同连接池大小是否都能保持业务正确性。
- `pool=4,8,16,32` 的吞吐和尾延迟差异。
- 提高 `pairs` 和 `client_workers` 后，连接池拐点是否从 `4` 或 `8` 转移到 `16`。

## 测试环境和配置

测试程序：

```text
./build/business_stress_test
```

真实后端配置：

```text
backend=real
base_config=config/stress.real.local.json
redis_enabled=false
password_hasher=fast
mysql=127.0.0.1:3306/chat_stress
```

说明：

- `real` 后端使用真实 MySQL 连接池、真实用户仓储和真实消息仓储。
- Redis 未启用，因此本次不覆盖 Redis session、缓存、限流和跨服推送。
- `password_hasher=fast` 不包含生产 bcrypt 成本，因此登录延迟不能代表生产密码哈希开销。
- 本次临时生成的连接池配置位于 `/tmp/stress-pool-configs/pool-{n}.json`，不应提交到 git。

临时配置生成方式：

```bash
mkdir -p /tmp/stress-pool-configs

python3 - <<'PY'
import json
from pathlib import Path

base = json.load(open("config/stress.real.local.json"))
out = Path("/tmp/stress-pool-configs")
out.mkdir(parents=True, exist_ok=True)

for n in [1, 2, 4, 8, 16, 32]:
    cfg = json.loads(json.dumps(base))
    cfg["mysql"]["pool"]["min_connections"] = n
    cfg["mysql"]["pool"]["max_connections"] = n
    cfg["mysql"]["pool"]["borrow_timeout_ms"] = 1000
    (out / f"pool-{n}.json").write_text(json.dumps(cfg, indent=2), encoding="utf-8")
PY
```

## 初始连接池扫描

初始扫描使用较小消息规模，快速观察连接池大小和吞吐、延迟之间的关系。

测试命令模板：

```bash
for pool in 1 2 4 8 16 32; do
  run_id="pool_${pool}_$(date +%Y%m%d%H%M%S)"

  env -u CHAT_DB_HOST -u CHAT_DB_PORT -u CHAT_DB_USER -u CHAT_DB_PASSWORD -u CHAT_DB_NAME \
    ./build/business_stress_test \
    --backend=real \
    --config=/tmp/stress-pool-configs/pool-${pool}.json \
    --run-id="$run_id" \
    --pairs=50 \
    --messages-per-pair=5 \
    --warmup-messages=1 \
    --client-workers=16 \
    | tee /tmp/stress-pool-${pool}.log
done
```

测试规模：

- `pairs=50`
- `messages_per_pair=5`
- `warmup_messages=1`
- `client_workers=16`
- 预期登录成功数：`100`
- 预期发送消息数：`250`
- 预期在线推送数：`250`
- 预期 ACK 数：`250`

测试结果：

| pool | status | msg/s | send p50 ms | send p95 ms | ack p50 ms | ack p95 ms | message duration ms |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | PASS | 100.84 | 78.66 | 103.10 | 74.70 | 94.76 | 2479.20 |
| 2 | PASS | 141.43 | 57.08 | 68.46 | 53.77 | 68.04 | 1767.62 |
| 4 | PASS | 254.98 | 29.46 | 39.63 | 27.98 | 45.13 | 980.45 |
| 8 | PASS | 248.46 | 30.73 | 37.28 | 29.78 | 40.19 | 1006.20 |
| 16 | PASS | 277.90 | 27.29 | 34.71 | 26.41 | 33.49 | 899.59 |
| 32 | PASS | 236.62 | 32.48 | 39.33 | 31.64 | 39.68 | 1056.56 |

初始扫描中，`pool=16` 单次结果最好，但 `pool=32` 已经回落。因此需要重复验证，避免把单次波动误判为稳定拐点。

## 三轮重复验证

重复验证只测试 `pool=4,8,16,32`，保持同一测试规模，连续运行三轮。

测试命令模板：

```bash
for round in 1 2 3; do
  for pool in 4 8 16 32; do
    run_id="pool_${pool}_r${round}_$(date +%Y%m%d%H%M%S)"

    env -u CHAT_DB_HOST -u CHAT_DB_PORT -u CHAT_DB_USER -u CHAT_DB_PASSWORD -u CHAT_DB_NAME \
      ./build/business_stress_test \
      --backend=real \
      --config=/tmp/stress-pool-configs/pool-${pool}.json \
      --run-id="$run_id" \
      --pairs=50 \
      --messages-per-pair=5 \
      --warmup-messages=1 \
      --client-workers=16 \
      | tee /tmp/stress-pool-${pool}-r${round}.log
  done
done
```

单轮结果：

| pool | round | status | msg/s | send p95 ms | ack p95 ms | message duration ms |
| ---: | ---: | --- | ---: | ---: | ---: | ---: |
| 4 | 1 | PASS | 258.47 | 37.53 | 35.29 | 967.22 |
| 4 | 2 | PASS | 280.42 | 35.41 | 32.99 | 891.53 |
| 4 | 3 | PASS | 258.80 | 34.95 | 34.45 | 966.01 |
| 8 | 1 | PASS | 246.99 | 36.54 | 35.39 | 1012.19 |
| 8 | 2 | PASS | 273.57 | 37.08 | 31.86 | 913.83 |
| 8 | 3 | PASS | 262.25 | 36.73 | 33.90 | 953.27 |
| 16 | 1 | PASS | 246.81 | 39.83 | 35.89 | 1012.94 |
| 16 | 2 | PASS | 244.50 | 39.54 | 36.57 | 1022.52 |
| 16 | 3 | PASS | 252.05 | 40.79 | 36.53 | 991.86 |
| 32 | 1 | PASS | 236.54 | 42.89 | 36.75 | 1056.89 |
| 32 | 2 | PASS | 247.08 | 37.61 | 37.03 | 1011.83 |
| 32 | 3 | PASS | 235.33 | 39.79 | 37.84 | 1062.35 |

汇总结果：

| pool | pass | avg msg/s | min msg/s | max msg/s | avg send p95 ms | avg ack p95 ms | avg message duration ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 3/3 | 265.90 | 258.47 | 280.42 | 35.96 | 34.24 | 941.59 |
| 8 | 3/3 | 260.94 | 246.99 | 273.57 | 36.78 | 33.72 | 959.76 |
| 16 | 3/3 | 247.79 | 244.50 | 252.05 | 40.05 | 36.33 | 1009.11 |
| 32 | 3/3 | 239.65 | 235.33 | 247.08 | 40.10 | 37.21 | 1043.69 |

三轮重复验证显示，`pool=4` 的平均吞吐最高，`pool=8` 接近，`pool=16` 和 `pool=32` 都没有稳定收益。初始扫描中 `pool=16` 的领先更像单次波动。

## 加压寻找拐点

为了确认更高并发下连接池拐点是否后移，继续提高 `pairs` 和 `client_workers`。根据重复验证结果，本阶段只测试 `pool=4,8,16`。由于 `pool=16` 没有明显变好，本次没有继续补测 `pool=32`。

测试场景：

- `pairs=100, client_workers=32`
- `pairs=200, client_workers=32`
- `pairs=200, client_workers=64`

测试命令模板：

```bash
for scenario in 100:32 200:32 200:64; do
  pairs=${scenario%%:*}
  workers=${scenario##*:}

  for pool in 4 8 16; do
    run_id="p${pairs}_w${workers}_pool_${pool}_$(date +%Y%m%d%H%M%S)"

    env -u CHAT_DB_HOST -u CHAT_DB_PORT -u CHAT_DB_USER -u CHAT_DB_PASSWORD -u CHAT_DB_NAME \
      ./build/business_stress_test \
      --backend=real \
      --config=/tmp/stress-pool-configs/pool-${pool}.json \
      --run-id="$run_id" \
      --pairs="$pairs" \
      --messages-per-pair=5 \
      --warmup-messages=1 \
      --client-workers="$workers" \
      | tee /tmp/stress-p${pairs}-w${workers}-pool-${pool}.log
  done
done
```

测试结果：

| 场景 | pool | status | expected messages | msg/s | send p95 ms | ack p95 ms | message duration ms |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| `pairs=100 workers=32` | 4 | PASS | 500 | 270.55 | 72.25 | 70.68 | 1848.10 |
| `pairs=100 workers=32` | 8 | PASS | 500 | 268.64 | 66.38 | 66.79 | 1861.26 |
| `pairs=100 workers=32` | 16 | PASS | 500 | 257.09 | 76.52 | 69.47 | 1944.85 |
| `pairs=200 workers=32` | 4 | PASS | 1000 | 275.27 | 66.91 | 66.16 | 3632.78 |
| `pairs=200 workers=32` | 8 | PASS | 1000 | 267.91 | 71.90 | 71.40 | 3732.58 |
| `pairs=200 workers=32` | 16 | PASS | 1000 | 255.99 | 79.47 | 72.91 | 3906.43 |
| `pairs=200 workers=64` | 4 | PASS | 1000 | 277.46 | 134.85 | 137.34 | 3604.14 |
| `pairs=200 workers=64` | 8 | PASS | 1000 | 269.12 | 139.98 | 140.28 | 3715.82 |
| `pairs=200 workers=64` | 16 | PASS | 1000 | 267.91 | 137.21 | 142.67 | 3732.58 |

按场景选择吞吐最高的连接池：

| 场景 | 最佳 pool | msg/s | send p95 ms | ack p95 ms |
| --- | ---: | ---: | ---: | ---: |
| `pairs=100 workers=32` | 4 | 270.55 | 72.25 | 70.68 |
| `pairs=200 workers=32` | 4 | 275.27 | 66.91 | 66.16 |
| `pairs=200 workers=64` | 4 | 277.46 | 134.85 | 137.34 |

加压后，`pool=16` 仍未表现出明显优势。`client_workers=64` 下整体吞吐相比 `client_workers=32` 只小幅变化，但 p95 延迟接近翻倍，说明增加客户端 worker 更多是在制造排队压力，而不是提升真实处理能力。

## 正确性结果

所有测试均满足以下条件：

- `status=PASS`
- `send_errors=0`
- `lost_push=0`
- `duplicate_push=0`
- `push_validation_errors=0`
- `ack_errors=0`
- `connect_errors=0`
- `protocol_errors=0`
- `server_errors=0`

这说明本次连接池对比中，业务正确性没有随连接池大小或客户端并发变化而退化。主要差异集中在吞吐和延迟。

## 结论

在当前本地真实 MySQL 后端、`messages_per_pair=5`、最多 `pairs=200` 和 `client_workers=64` 的压测范围内，推荐连接池大小为：

```text
min_connections=4
max_connections=4
```

判断依据：

- 三轮重复验证中，`pool=4` 平均吞吐最高，为 `265.90 msg/s`。
- 加压到 `pairs=200, client_workers=64` 后，`pool=4` 仍是吞吐最高，为 `277.46 msg/s`。
- `pool=8` 与 `pool=4` 接近，但没有稳定吞吐优势。
- `pool=16` 在重复验证和加压测试中都没有稳定收益。
- `pool=32` 在初始扫描和重复验证中都没有优势，因此加压阶段未继续测试。

当前观察到的拐点更接近 `pool=4`。继续增大连接池可能增加 MySQL 连接调度、锁竞争或数据库侧并发开销，而不是提升业务吞吐。

## 后续建议

- 将真实后端压测配置的默认 MySQL 连接池大小暂定为 `4..4`。
- 如果后续业务链路增加批量写入、更多查询或 Redis 后端，需要重新跑同类连接池扫描。
- 下一轮可扩大到 `pairs=500` 或更高消息量，验证 `pool=4` 是否仍然稳定。
- 增加压测程序的 `--quiet` 或日志级别控制，避免大量连接日志影响报告阅读和终端输出。
- 后续 benchmark 固定记录 MySQL 版本、CPU、磁盘、构建类型和数据库参数，便于跨机器、跨提交比较。
