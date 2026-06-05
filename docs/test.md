# Redis 测试计划

## 测试目标

本文档描述项目引入 Redis 中间件后的分阶段测试集。测试设计遵循以下原则：

- MySQL 继续作为用户和消息数据的事实来源。
- Redis 主要承担临时状态、缓存、限流、幂等短缓存和跨进程推送协调。
- 单元测试默认使用 fake Redis，不依赖本地 Redis 服务。
- 真实 Redis 集成测试通过环境变量显式开启，避免影响普通开发和 CI。
- 消息状态语义保持不变：只有响应或推送成功进入目标连接发送队列后，消息才能标记为 `delivered`。

## 测试分层

推荐按以下层次组织测试：

- 基础设施测试：验证 Redis 配置、连接、连接池和命令封装。
- 组件单元测试：使用 fake Redis 验证 SessionStore、缓存、限流和 Stream 逻辑。
- Service 测试：验证 Redis 能力接入后，`UserService` 和 `ChatService` 的业务行为。
- Handler 测试：验证请求解析、响应封装和延迟副作用仍符合现有线程边界。
- 集成测试：在真实 Redis 或多 server 进程下验证端到端行为。
- 回归测试：每阶段都必须跑现有测试，确保 Redis 接入不破坏当前能力。

## 阶段 0：Redis 基础设施

### 建议新增测试文件

```text
tests/redis_config_test.cc
tests/redis_connection_test.cc
tests/redis_pool_test.cc
tests/fake_redis_client.h
```

### RedisConfig 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 默认配置 | Redis 环境变量全部为空 | 使用默认 host、port、db、pool size 和 timeout |
| 全量覆盖 | 设置 host、port、password、db、pool size、timeout | 所有字段按环境变量解析 |
| 禁用 Redis | `CHAT_REDIS_ENABLED=0` | 服务不初始化 Redis，保持本地路径 |
| 非法 port | port 非数字或超出 `uint16_t` | 返回配置错误 |
| 非法 db | db 小于 0 | 返回配置错误 |
| 非法 pool size | pool size 为 0 或非数字 | 返回配置错误 |
| 非法 timeout | timeout 为 0、负数或非数字 | 返回配置错误 |
| key prefix 为空 | 未配置 `CHAT_REDIS_KEY_PREFIX` | 使用默认前缀 `chat` |
| server id 为空 | 未配置 `CHAT_SERVER_ID` | 自动生成或返回配置错误，行为必须固定 |

### RedisConnection 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| ping 成功 | 正常连接后执行 `PING` | 返回 OK |
| 连接失败 | Redis 地址不可达 | 返回连接不可用，不崩溃 |
| auth 成功 | 配置 password 且认证正确 | 初始化成功 |
| auth 失败 | password 错误 | 初始化失败并带错误信息 |
| select db 成功 | 指定 db 存在 | 初始化成功 |
| select db 失败 | db 不可用或命令失败 | 初始化失败 |
| 命令超时 | Redis 响应超过 command timeout | 返回 timeout 状态 |
| error reply | Redis 返回 error reply | 映射为命令失败 |
| nil reply | 查询不存在 key | 映射为 not found |
| 类型错误 | 对错误类型 key 执行命令 | 返回命令失败 |
| 断线重连 | 连接中途断开后重试 | 重新建立连接或标记 bad connection |

### RedisPool 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 初始化成功 | pool size 为 N | 创建 N 个可用连接 |
| 初始化部分失败 | 某些连接创建失败 | 初始化失败或按设计降级，行为固定 |
| 借还连接 | borrow 后归还 | 连接可复用 |
| 连接耗尽 | 所有连接已借出 | 等待后超时 |
| bad connection 归还 | 连接被标记不可用 | 不再复用该连接 |
| 多线程借还 | 多线程并发 borrow/return | 无死锁、无重复借出 |
| 析构安全 | pool 析构时仍有连接归还 | 不崩溃 |
| 统计信息 | 查询空闲数、使用中数量 | 统计值正确 |

### 真实 Redis 集成测试开关

真实 Redis 测试默认不运行，建议通过以下环境变量开启：

```bash
CHAT_REDIS_TEST_ENABLED=1
CHAT_REDIS_TEST_HOST=127.0.0.1
CHAT_REDIS_TEST_PORT=6379
CHAT_REDIS_TEST_PASSWORD=
CHAT_REDIS_TEST_DB=15
```

CTest 中建议为真实 Redis 测试添加 label：

```text
redis_integration
```

## 阶段 1：会话、Token 和在线状态

### 建议新增测试文件

```text
tests/redis_session_store_test.cc
tests/session_manager_with_redis_test.cc
tests/user_service_redis_session_test.cc
tests/message_handler_redis_session_test.cc
```

### RedisSessionStore 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 保存 token session | 写入用户 token 会话 | `chat:session:token:{token}` 存在并带 TTL |
| 查询 token session | token 存在 | 返回 user id、username 和 token |
| token 不存在 | 查询未知 token | 返回 not found |
| token 过期 | TTL 到期后查询 | 返回 not found |
| 删除 token | 主动删除 token | 后续查询不到 |
| 保存 presence | 用户绑定到本机连接 | user presence 和 conn presence 都写入 |
| 刷新 presence | 心跳或有效请求续期 | TTL 被刷新 |
| 清理 presence | logout 或连接关闭 | 两类 presence key 都删除 |
| 重复登录 | 同一 user id 绑定新连接 | 新 presence 覆盖旧 presence |
| 连接换绑 | 同一 connection id 绑定不同用户 | 旧用户 presence 被清理 |
| JSON 损坏 | Redis 中 session 数据格式错误 | 返回解析失败并清理坏 key |
| 写入失败 | Redis 命令失败 | 返回明确错误，不污染本地 session |
| 删除失败 | Redis 删除命令失败 | 返回错误，调用方仍能清理本地 session |

### SessionManager 与 Redis 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 绑定成功 | `BindSession` 绑定合法 session | 本地 map 和 Redis presence 都存在 |
| 非法 session | user id 非正数或 username 为空 | 绑定失败，不写 Redis |
| 清理成功 | `ClearSession` 清理已登录连接 | 本地 map 删除，Redis presence 删除 |
| 重复绑定同一用户 | user id 已在旧连接登录 | 旧连接本地 session 被移除，新连接生效 |
| 连接换绑用户 | connection id 已属于其他用户 | 旧用户本地映射被清理 |
| Redis 写失败 | 本地绑定成功但 Redis 写失败 | 按策略返回失败或降级成功，行为必须固定 |
| Redis 删除失败 | 连接关闭时 Redis 删除失败 | 本地必须清理，错误只记录日志 |
| 本机 GetSession | 根据 connection id 查询 | 从本地返回 session |
| 本机 GetConnectionId | 根据 user id 查询 | 返回本地 connection id |
| 远端 presence | Redis 中只有其他 server 的 presence | 第一阶段返回空，不能误用远端 connection id |

### UserService 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 登录成功 | 用户名密码正确 | 返回随机 token，不再是 `token_{user_id}` |
| 登录后绑定 | I/O 线程执行 bind action | token session 和 presence 写入 Redis |
| 密码错误 | 密码不匹配 | 不写 Redis |
| 用户不存在 | 用户查询返回 not found | 不写 Redis |
| MySQL 查询失败 | 用户查询失败 | 返回数据库错误，不写 Redis |
| 登出成功 | 当前连接已登录 | 返回 OK，并在 unbind 后清理 Redis |
| 登出未登录 | 当前连接无 session | 返回未登录，不产生 Redis 副作用 |
| whoami 成功 | 当前连接已登录 | 返回本地 session 信息 |
| whoami 未登录 | 当前连接无 session | 返回未登录 |
| 连接关闭 | 网络层通知连接关闭 | 清理本地 session 和 Redis presence |
| 心跳续期 | 已登录连接发送 heartbeat | presence TTL 被刷新 |

### Handler 和端到端测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| login 到 whoami | 同一连接登录后查询身份 | 返回当前用户和 token |
| login 到 logout 到 send_message | 登出后发送消息 | 返回未登录 |
| 登录响应前连接断开 | worker 已生成 `SessionAction::BIND`，I/O 回投时连接失效 | 不执行 Redis bind |
| logout 响应前连接断开 | worker 已生成 `SessionAction::UNBIND`，I/O 回投时连接失效 | 本地关闭路径仍清理连接态 |
| Redis 关闭且 disabled | `CHAT_REDIS_ENABLED=0` | 服务可启动，本地 session 测试通过 |
| Redis 关闭且 strict enabled | Redis 必需但不可连接 | 服务启动失败 |
| 重复登录 | 同一用户两个连接先后登录 | 最新连接有效，旧连接不再接收该用户推送 |

## 阶段 2：缓存、限流和幂等短缓存

### 建议新增测试文件

```text
tests/cached_user_repository_test.cc
tests/redis_rate_limiter_test.cc
tests/message_dedup_cache_test.cc
tests/chat_service_cache_test.cc
```

### CachedUserRepository 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| id 缓存 miss | `findById` 未命中 Redis | 回源 MySQL fake，并写 Redis |
| id 缓存 hit | `findById` 命中 Redis | 不访问 MySQL fake |
| username 缓存 miss | `findByUsername` 未命中 Redis | 回源 MySQL fake，并写 username 到 id 映射 |
| username 缓存 hit | username 映射和 id 数据都存在 | 不访问 MySQL fake |
| 缓存数据损坏 | Redis 中 JSON 或 Hash 不合法 | 删除坏缓存，回源 MySQL |
| not found 空缓存 | MySQL 返回 not found | 写短 TTL 空缓存，避免穿透 |
| 空缓存命中 | Redis 命中 not found 标记 | 直接返回 not found |
| 创建用户成功 | `createUser` 成功 | 删除或更新相关 username/id 缓存 |
| 创建用户重复 | MySQL 返回 duplicate | 不写成功缓存 |
| Redis 不可用 | Redis 查询或写入失败 | 直接回源 MySQL，业务不失败 |
| 缓存过期 | TTL 到期后再次查询 | 再次回源 MySQL |
| 并发查询 | 多线程查询同一用户 | 不返回错用户，不破坏缓存格式 |

### RedisRateLimiter 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 窗口内允许 | 未超过限制 | 返回 allow |
| 超过限制 | 第 N+1 次请求 | 返回 reject，并带剩余 TTL |
| 窗口过期 | TTL 到期后再次请求 | 重新 allow |
| 用户隔离 | 不同 user id 请求 | 计数互不影响 |
| IP 隔离 | 不同客户端 IP 请求 | 计数互不影响 |
| 操作隔离 | login、register、send_message 分别限流 | 不同操作类型互不影响 |
| INCR 成功 EXPIRE 失败 | 首次计数后设置 TTL 失败 | 返回失败或删除 key，避免永久限流 |
| Redis 不可用 | Redis 命令失败 | 按策略 fail open 或 fail closed，行为固定 |
| 并发限流 | 多线程同时请求同一 key | 最多允许配置数量 |
| TTL 查询失败 | reject 时查询 TTL 失败 | 仍返回 reject，并使用默认 retry after |

### MessageDedupCache 测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 首次发送 | client msg id 未命中 Redis | 继续走 MySQL 创建消息 |
| 写入 dedup | MySQL 创建成功 | 写入 `from_user_id + client_msg_id -> message_id` |
| Redis 命中 | 重复 client msg id 命中 Redis | 可短路读取已有 message id |
| Redis 命中但 MySQL 不一致 | Redis 指向记录与请求内容冲突 | 以 MySQL 为准，返回幂等冲突 |
| Redis miss 但 MySQL duplicate | Redis 没有缓存，MySQL 命中唯一约束 | 正确返回已有消息 |
| Redis 不可用 | dedup 查询或写入失败 | MySQL 幂等约束仍生效 |
| TTL 过期 | dedup key 过期后重复请求 | MySQL 仍能兜底 |
| 并发重复发送 | 同一 user id 和 client msg id 并发发送 | 只产生一条 MySQL 消息 |

### ChatService 缓存集成测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 目标用户缓存 miss | 发送给存在用户 | 发送成功并写用户缓存 |
| 目标用户缓存 hit | 发送给存在用户 | 不访问 user repository fake |
| 目标用户空缓存 hit | 发送给不存在用户 | 返回 `USER_NOT_FOUND` |
| 发消息限流通过 | 未超过限制 | 继续落库发送 |
| 发消息限流触发 | 超过限制 | 不写消息表 |
| 缓存故障 | Redis 缓存不可用 | 发送消息仍按 MySQL 路径成功 |
| dedup 命中 | 重复发送相同 client msg id | 返回已有消息语义 |
| dedup 冲突 | 相同 client msg id 携带不同内容 | 返回 `IDEMPOTENCY_CONFLICT` |

## 阶段 3：跨进程推送和 Redis Stream

### 建议新增测试文件

```text
tests/redis_push_stream_test.cc
tests/cross_server_push_router_test.cc
tests/cross_server_delivery_test.cc
tests/multi_server_integration_test.cc
```

### Presence 路由测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 本机在线 | 目标用户 presence 指向当前 server id | 走本地 `HandleResult.pushes` |
| 远端在线 | 目标用户 presence 指向其他 server id | 写入目标 server 的 Redis Stream |
| 离线用户 | 目标用户无 presence | 不写 stream，消息保持 `stored` |
| presence 过期 | presence TTL 到期 | 视为离线 |
| presence 格式错误 | server id 或 connection id 缺失 | 视为离线并记录日志 |
| 目标连接为 0 | presence 中 connection id 非法 | 视为离线 |
| 发送者和接收者跨 server | user1 在 server A，user2 在 server B | 消息落库后写入 server B stream |

### Stream 生产者测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| XADD 成功 | 写入目标 server stream | 返回 stream id |
| XADD 失败 | Redis 命令失败 | 不标记 delivered |
| payload 完整 | 写入 push payload | 包含 message id、to user id、target connection id、payload |
| 重复写入 | 同一 message id 重复写入 | 消费端能够幂等处理 |
| Redis 不可用 | 远端在线但 stream 写失败 | 消息仍已 MySQL stored，可离线拉取 |
| stream key 错误 | server id 包含非法字符或为空 | 返回配置错误或路由失败 |

### Stream 消费者测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 消费成功 | 读取到远端 push 事件 | 找到本地连接并入发送队列 |
| 投递成功 | 入发送队列成功 | 调用 `markDelivered` |
| 目标连接不存在 | connection id 不在本地 | 不投递，不标记 delivered |
| 目标连接换绑 | connection id 已属于其他用户 | 不投递，不标记 delivered |
| payload 解析失败 | stream 消息字段缺失或格式错误 | ack 或进入死信策略，不能无限阻塞 |
| markDelivered 失败 | MySQL 更新失败 | ack 策略固定，建议记录重试任务 |
| 消费者重启 | 有 pending stream 消息 | 可以恢复处理 |
| 重复消费 | 同一 stream 消息被重复读取 | 最多投递一次到有效连接，`markDelivered` 幂等 |
| 消费超时 | XREADGROUP 超时 | 正常继续轮询，不占用 CPU 空转 |

### 双 server 集成测试集

| 用例 | 场景 | 预期 |
| --- | --- | --- |
| 跨 server 在线推送 | server A 登录 user1，server B 登录 user2，user1 发给 user2 | user2 收到 push，MySQL 状态变为 delivered |
| 写 stream 后断开 | user2 在 server B 消费前断开 | 不收到 push，消息保持 stored 或可离线拉取 |
| 断开后重连 | user2 重连到 server A | 离线拉取能拿到未 delivered 消息 |
| 消费者崩溃恢复 | server B 消费前退出再启动 | 恢复后继续处理 pending |
| stream 写失败 | server A 写 Redis Stream 失败 | user1 收到发送成功 ack，user2 后续可离线拉取 |
| 重复登录竞争 | user2 同时在两个 server 登录 | presence 最终指向最新连接，旧连接不应收到消息 |
| Redis 重启 | 推送期间 Redis 短暂不可用 | 服务不崩溃，消息可通过 MySQL 离线路径恢复 |

## 每阶段必须保留的回归测试

每个阶段完成后都必须运行现有测试：

```bash
cmake --build build
ctest --test-dir build --output-on-failure
```

重点关注以下目标：

```text
session_manager_test
user_service_test
chat_service_test
message_handler_test
auth_integration_test
message_repository_test
server_integration_test
```

## 测试实现建议

- fake Redis 组件应支持 key-value、hash、TTL、INCR、DEL、EXPIRE、XADD 和 XREADGROUP 的最小行为。
- fake Redis 需要能注入连接失败、命令失败、超时、损坏数据和过期行为。
- 真实 Redis 集成测试应使用独立 db，并在测试前后清理 `chat:*` key。
- Redis 故障降级策略必须在测试中固定，不能让同一错误在不同调用路径表现不一致。
- 缓存类测试必须验证 cache hit 时底层 Repository 没有被调用。
- 消息链路测试必须验证 Redis 失败不会提前把消息标记为 `delivered`。
- 跨 server 测试应显式设置不同 `CHAT_SERVER_ID`，避免使用随机 server id 造成断言不稳定。
