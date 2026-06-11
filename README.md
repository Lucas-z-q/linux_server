# Linux Chat Server

基于 C++17、Linux Socket、epoll、线程池、MySQL 和 Redis 实现的 TCP 长连接聊天服务器。
项目用于学习和演进服务端网络编程、协议处理、会话管理、消息投递与持久化。

## 功能概览

### 用户与认证

- 用户注册、用户名查重和 MySQL 持久化
- 用户登录、密码校验和随机 token 生成
- 基于 TCP 连接的登录态管理
- 登出、断开连接后的会话清理
- `whoami` 登录态查询
- 心跳响应与 Redis 在线状态续期

### 单聊消息

- 单聊文本消息持久化
- 在线用户实时 `message_push`
- 离线消息拉取
- 基于 `client_msg_id` 的发送幂等
- 消息状态 `stored`、`delivered` 和 `read`
- Redis 启用时支持跨服务器实例推送

### 服务端基础设施

- epoll 非阻塞 I/O 和 TCP 长连接
- 线程池异步执行请求
- 同一连接上的请求顺序处理
- 收发缓冲、半包和粘包处理
- 空闲连接及心跳超时清理
- MySQL 与 Redis 连接池
- Redis 用户缓存、消息去重、限流和在线状态
- 配置文件与环境变量覆盖
- 同步或异步日志、文件滚动
- 单元测试、集成测试和真实依赖集成测试

## 技术栈

- CMake
- C++17
- Linux Socket、epoll、eventfd、timerfd
- MySQL Client
- hiredis
- nlohmann/json
- GoogleTest、CTest

## 架构

核心请求链路如下：

```text
client
  -> TcpServer / ConnectionContext / PacketCodec
  -> IMessageHandler / MessageHandler / JsonCodec
  -> UserService / ChatService / SessionManager
  -> UserRepository / MessageRepository
  -> DbPool / MySQL
```

启用 Redis 后，会额外接入：

```text
RedisPool / RedisClient
  -> CachedUserRepository
  -> RedisSessionStore
  -> RedisRateLimiter
  -> MessageDedupCache
  -> RedisPushStream
```

详细目录和模块职责见 [docs/project_structure.md](docs/project_structure.md)。

## 构建

需要支持 C++17 的编译器、CMake、MySQL Client 开发库和 pthread。系统中未安装 GoogleTest 或 hiredis
时，CMake 会尝试通过 `FetchContent` 下载。

```bash
cmake -S . -B build
cmake --build build
```

构建结果包括：

- `build/server`：聊天服务器
- `build/client`：简单的单请求 TCP 客户端
- `build/*_test`：测试程序

## 数据库初始化

先创建目标数据库，然后执行 schema：

```bash
mysql -u root -p -e "CREATE DATABASE IF NOT EXISTS chat CHARACTER SET utf8mb4;"
mysql -u root -p chat < sql/001_create_chat_tables.sql
```

脚本会创建：

- `users`
- `conversations`
- `conversation_members`
- `messages`

## 配置

复制示例配置并按本地环境修改：

```bash
cp config/server.example.json config/server.json
```

配置文件包含监听地址、MySQL、Redis、日志和连接超时等参数。配置文件查找优先级为：

1. `--config` 命令行参数
2. `CHAT_CONFIG_PATH` 环境变量
3. `config/server.json`

例如：

```bash
./build/server --config config/server.json
```

常用 MySQL 环境变量：

| 环境变量 | 作用 |
| --- | --- |
| `CHAT_DB_HOST` | MySQL 主机 |
| `CHAT_DB_PORT` | MySQL 端口 |
| `CHAT_DB_USER` | MySQL 用户名 |
| `CHAT_DB_PASSWORD` | MySQL 密码 |
| `CHAT_DB_NAME` | MySQL 数据库名 |

常用 Redis 环境变量：

| 环境变量 | 作用 |
| --- | --- |
| `CHAT_REDIS_ENABLED` | 是否启用 Redis，取值为 `0` 或 `1` |
| `CHAT_REDIS_HOST` | Redis 主机 |
| `CHAT_REDIS_PORT` | Redis 端口 |
| `CHAT_REDIS_PASSWORD` | Redis 密码 |
| `CHAT_REDIS_DB` | Redis 数据库编号 |
| `CHAT_REDIS_KEY_PREFIX` | Redis key 前缀 |
| `CHAT_SERVER_ID` | 当前服务实例 ID |

连接超时可通过 `CHAT_CONNECTION_IDLE_TIMEOUT_MS` 和 `CHAT_HEARTBEAT_TIMEOUT_MS` 覆盖。
完整配置项参考 [config/server.example.json](config/server.example.json) 和
[src/config/config_loader.cc](src/config/config_loader.cc)。

Redis 是可选组件。关闭 Redis 后，注册、登录、单实例会话、消息持久化、在线推送和离线拉取仍可运行；
用户缓存、分布式会话状态、限流、Redis 去重和跨实例推送不会启用。

## 运行

启动服务器：

```bash
./build/server
```

默认监听 `127.0.0.1:8080`。

简单客户端从标准输入读取一条 JSON 请求，发送后打印一条响应：

```bash
./build/client
```

也可以使用 `nc` 手动测试：

```bash
printf '%s\n' \
  '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}' \
  | nc 127.0.0.1 8080
```

## 协议

### 传输规则

- 使用 TCP 长连接
- 字符编码为 UTF-8
- 每条消息是一个 JSON 对象
- 每个 JSON 对象后必须追加换行符 `\n`
- 单个请求和对应响应使用相同的 `seq`
- 服务端主动推送的 `seq` 为 `0`

客户端请求信封：

```json
{
  "msg_type": "login",
  "seq": 1,
  "token": "",
  "data": {}
}
```

服务端响应信封：

```json
{
  "msg_type": "login_resp",
  "seq": 1,
  "code": 0,
  "message": "login success",
  "data": {}
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `msg_type` | string | 消息类型 |
| `seq` | integer | 客户端请求序号 |
| `token` | string | 可选 token 字段 |
| `data` | object | 业务数据 |
| `code` | integer | 响应错误码，`0` 表示成功 |
| `message` | string | 响应描述 |

当前服务端认证主要依赖连接绑定的会话。登录后应继续使用同一条 TCP 连接发送需要认证的请求。
协议保留 `token`，Redis 启用时服务端会存储和撤销 token，但当前版本不会使用请求信封中的 token
恢复连接登录态或对每个请求重新鉴权。

### 消息类型

| 请求类型 | 响应类型 | 是否需要登录 |
| --- | --- | --- |
| `register` | `register_resp` | 否 |
| `login` | `login_resp` | 否 |
| `logout` | `logout_resp` | 是 |
| `heartbeat` | `heartbeat_resp` | 否 |
| `whoami` | `whoami_resp` | 是 |
| `send_message` | `send_message_resp` | 是 |
| `pull_offline_messages` | `pull_offline_messages_resp` | 是 |

### 注册

```json
{
  "msg_type": "register",
  "seq": 1,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456",
    "nickname": "Alice"
  }
}
```

成功响应：

```json
{
  "msg_type": "register_resp",
  "seq": 1,
  "code": 0,
  "message": "register success",
  "data": {
    "user_id": 10001
  }
}
```

`username` 和 `password` 必填，`nickname` 可选。用户名必须唯一。

### 登录

```json
{
  "msg_type": "login",
  "seq": 2,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456"
  }
}
```

成功响应：

```json
{
  "msg_type": "login_resp",
  "seq": 2,
  "code": 0,
  "message": "login success",
  "data": {
    "user_id": 10001,
    "nickname": "Alice",
    "token": "64-character-random-token"
  }
}
```

登录成功后，服务端将用户会话绑定到当前连接。同一用户重新绑定连接时，本地会话映射只保留最新连接。

### 登出

```json
{
  "msg_type": "logout",
  "seq": 3,
  "token": "",
  "data": {}
}
```

成功登出会清理本地会话。启用 Redis 时还会清理在线状态并撤销 token。

### 心跳

```json
{
  "msg_type": "heartbeat",
  "seq": 4,
  "token": "",
  "data": {}
}
```

响应：

```json
{
  "msg_type": "heartbeat_resp",
  "seq": 4,
  "code": 0,
  "message": "Heartbeat received",
  "data": {
    "server_time": 1710000000
  }
}
```

任意格式合法的请求都会更新连接活跃时间；已登录连接还会尝试续期 Redis 在线状态。

### 查询当前用户

```json
{
  "msg_type": "whoami",
  "seq": 5,
  "token": "",
  "data": {}
}
```

成功响应：

```json
{
  "msg_type": "whoami_resp",
  "seq": 5,
  "code": 0,
  "message": "ok",
  "data": {
    "user_id": 10001,
    "username": "alice",
    "token": "64-character-random-token"
  }
}
```

### 发送单聊消息

```json
{
  "msg_type": "send_message",
  "seq": 10,
  "token": "",
  "data": {
    "client_msg_id": "client-msg-001",
    "to_user_id": 10002,
    "content": "hello bob"
  }
}
```

字段约束：

| 字段 | 约束 |
| --- | --- |
| `client_msg_id` | 非空，最长 64 字符；同一发送者范围内唯一 |
| `to_user_id` | 大于 `0`，目标用户必须存在，不能是发送者自己 |
| `content` | 非空，最长 4096 字符 |

发送确认响应：

```json
{
  "msg_type": "send_message_resp",
  "seq": 10,
  "code": 0,
  "message": "Success",
  "data": {
    "message_id": "msg_0000000065f000000000000000000001",
    "conversation_id": "conv_10001_10002",
    "to_user_id": 10002,
    "status": 0,
    "created_at": 1710000000
  }
}
```

相同 `client_msg_id` 和相同消息内容的重试会返回已有消息；相同 `client_msg_id` 对应不同消息内容时，
服务端返回幂等冲突。

目标用户在线时，服务端主动推送：

```json
{
  "msg_type": "message_push",
  "seq": 0,
  "code": 0,
  "message": "new message",
  "data": {
    "message_id": "msg_0000000065f000000000000000000001",
    "conversation_id": "conv_10001_10002",
    "from_user_id": 10001,
    "from_username": "alice",
    "to_user_id": 10002,
    "content": "hello bob",
    "created_at": 1710000000,
    "server_time": 1710000000
  }
}
```

### 拉取离线消息

```json
{
  "msg_type": "pull_offline_messages",
  "seq": 11,
  "token": "",
  "data": {
    "limit": 50,
    "since_message_id": ""
  }
}
```

`limit` 必须在 `1` 到 `100` 之间。`since_message_id` 可用于向后拉取；当前版本不支持
`before_message_id`，传入非空值会返回参数错误。

成功响应：

```json
{
  "msg_type": "pull_offline_messages_resp",
  "seq": 11,
  "code": 0,
  "message": "Success",
  "data": {
    "messages": [
      {
        "message_id": "msg_001",
        "conversation_id": "conv_10001_10002",
        "from_user_id": 10002,
        "to_user_id": 10001,
        "content": "offline message",
        "created_at": 1710000000,
        "status": 0
      }
    ],
    "has_more": false
  }
}
```

在线 push 或离线拉取响应成功进入 I/O 发送队列后，服务端将对应消息推进为 `delivered`。

### 消息状态

| 值 | 名称 | 含义 |
| --- | --- | --- |
| `0` | `stored` | 已持久化，尚未由服务端投递 |
| `1` | `delivered` | 已进入目标连接或离线拉取响应的发送队列 |
| `2` | `read` | 已读状态，领域模型和数据库已预留 |

`delivered` 只表示服务端完成投递，不表示客户端已收到或用户已阅读。当前协议尚未实现客户端已读上报。

## 错误码

| 错误码 | 枚举 | 含义 |
| --- | --- | --- |
| `0` | `OK` | 成功 |
| `1` | `INVALID_PARAM` | 参数或协议信封无效 |
| `2` | `INVALID_JSON` | JSON 无效 |
| `3` | `INVALID_PACKET` | 数据包无效 |
| `4` | `UNKNOWN_MESSAGE_TYPE` | 未知消息类型 |
| `1001` | `USER_ALREADY_EXISTS` | 用户名已存在 |
| `1002` | `USER_NOT_FOUND` | 用户不存在或当前连接未登录 |
| `1003` | `WRONG_PASSWORD` | 密码错误，当前登录流程未直接返回此码 |
| `1004` | `INVALID_CREDENTIALS` | 用户名或密码错误 |
| `1005` | `USER_ALREADY_ONLINE` | 用户已在线，当前流程预留 |
| `2001` | `DB_INIT_FAILED` | 数据库初始化失败 |
| `2002` | `DB_QUERY_FAILED` | 数据库查询失败 |
| `2003` | `DB_INSERT_FAILED` | 数据库写入失败 |
| `3001` | `MESSAGE_TOO_LONG` | 消息内容过长 |
| `3002` | `USER_NOT_ONLINE` | 用户不在线，当前发送流程会保存离线消息 |
| `3003` | `CANNOT_SEND_TO_SELF` | 不能给自己发送消息 |
| `3004` | `NOT_LOGGED_IN` | 当前连接未登录 |
| `3005` | `IDEMPOTENCY_CONFLICT` | 幂等键对应的消息内容冲突 |
| `3006` | `RATE_LIMITED` | 请求触发 Redis 限流 |
| `9001` | `INTERNAL_ERROR` | 服务端内部错误 |

触发限流时，响应 `data.retry_after_seconds` 会给出建议重试时间。

## 测试

运行全部已注册测试：

```bash
ctest --test-dir build --output-on-failure
```

真实 MySQL 和 Redis 集成测试需要额外环境变量，未配置时相关用例会跳过。测试配置和测试矩阵见
[docs/test.md](docs/test.md)。

提交前执行：

```bash
clang-format -i $(git ls-files '*.cc' '*.h')
cpplint $(git ls-files '*.cc' '*.h')
```

## 当前边界

- 密码目前使用 `std::hash`，只适合学习和演示，生产环境应替换为 Argon2、bcrypt 或 scrypt。
- token 使用 `getrandom` 生成，但尚未实现基于 token 的断线重连和逐请求鉴权。
- 消息协议目前只支持单聊文本，不支持好友关系、群聊、图片和文件。
- `read` 状态尚无客户端上报协议。
- 应用层分包仍使用换行符，后续可升级为长度前缀协议。
- Redis 启用时，启动阶段连接或 Push Stream 初始化失败会导致服务器启动失败。
