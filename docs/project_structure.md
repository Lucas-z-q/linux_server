# 项目结构说明

## 项目概览

本项目是一个基于 C++17 的 TCP 长连接聊天服务器。当前代码已经覆盖用户注册、登录、断线恢复、登出、心跳、登录态查询、单聊、好友关系、群聊、小规模群发、离线消息、消息 ACK、已读回执、会话内消息序号、本地多端在线、MySQL 持久化和可选 Redis 分布式能力。

核心请求链路如下：

```text
client
  -> TcpServer / ConnectionContext / PacketCodec
  -> IMessageHandler / MessageHandler / JsonCodec
  -> UserService / ChatService / FriendService / GroupService / SessionManager
  -> UserRepository / MessageRepository / FriendRepository / GroupRepository
  -> DbPool / MysqlStatement / DbConnection
  -> MySQL
```

启用 Redis 后额外接入：

```text
RedisPool / RedisClient
  -> CachedUserRepository
  -> RedisSessionStore
  -> RedisRateLimiter
  -> MessageDedupCache
  -> RedisPushStream
```

协议使用换行分隔 JSON。所有业务消息统一使用信封字段 `msg_type`、`seq`、`token`、`data`，响应增加 `code` 和 `message`。

## 目录结构

```text
.
|-- CMakeLists.txt
|-- README.md
|-- client/
|   `-- client.cc
|-- config/
|   |-- server.example.json
|   `-- server.json
|-- docs/
|   |-- project_structure.md
|   `-- test.md
|-- include/
|   |-- app/
|   |-- cache/
|   |-- codec/
|   |-- common/
|   |-- concurrency/
|   |-- config/
|   |-- db/
|   |-- handler/
|   |-- model/
|   |-- net/
|   |-- protocol/
|   |-- redis/
|   |-- security/
|   |-- server/
|   |-- service/
|   `-- stream/
|-- sql/
|   `-- 001_create_chat_tables.sql
|-- src/
|   |-- app/
|   |-- cache/
|   |-- codec/
|   |-- common/
|   |-- concurrency/
|   |-- config/
|   |-- db/
|   |-- handler/
|   |-- protocol/
|   |-- redis/
|   |-- security/
|   |-- server/
|   |-- service/
|   |-- stream/
|   |-- main.cc
|   |-- TcpServer.cc
|   |-- TcpConnection.cc
|   `-- ConnectionContext.cc
|-- tests/
|   |-- *_test.cc
|   |-- *_integration_test.cc
|   |-- fake_*repository.h
|   `-- in_memory_session_store.h
`-- third_party/
    `-- nlohmann/
```

`build/`、`Testing/`、`.git/`、`.vscode/`、`.codex/`、`.claude/` 等目录属于本地生成物或开发环境文件，不属于源码结构。

## 构建目标

根目录 `CMakeLists.txt` 负责项目设置和子目录装配，第三方依赖集中在 `cmake/Dependencies.cmake`，生产源码、客户端和测试分别由 `src/CMakeLists.txt`、`client/CMakeLists.txt` 和 `tests/CMakeLists.txt` 管理。

- `chat_common`：日志、输入校验和密码哈希。
- `chat_codec`：TCP 分包、JSON 编解码和协议辅助。
- `chat_net`：连接上下文、epoll 服务端和线程池。
- `chat_db`：MySQL 连接、连接池和仓储实现。
- `chat_redis`：Redis 客户端、缓存、全局会话和推送流。
- `chat_service`：本地会话管理和业务服务。
- `chat_handler`：协议路由和业务编排。
- `chat_server_app`：配置加载和启动辅助。
- `server`：只编译 `src/main.cc`，链接上述生产库。
- `client`：简单 TCP 客户端，只链接 `chat_codec`。
- `*_test`：只编译测试自身源码，并链接需要的生产库。

兼容 alias `chat_logger`、`security_runtime` 和 `redis_runtime` 分别指向 `chat_common`、`chat_common` 和 `chat_redis`。仓库内部的新构建声明统一使用 `chat_*` 目标。

主要依赖包括 POSIX threads、MySQL client、hiredis、GoogleTest 和 nlohmann/json。系统缺少 GoogleTest 或 hiredis 时，CMake 会尝试通过 `FetchContent` 构建。

## 入口与装配

文件：

- `src/main.cc`
- `include/app/main_runner.h`
- `src/app/main_runner.cc`
- `include/config/*.h`
- `src/config/*.cc`

职责：

- 解析 `--config`、`CHAT_CONFIG_PATH` 和环境变量覆盖项。
- 校验 server、mysql、redis、log、timeout、connection、heartbeat 配置。
- 初始化 Logger、MySQL 连接池、Redis 连接池和业务依赖。
- 按 `Repository -> Service -> MessageHandler -> TcpServer` 顺序装配对象。
- 在启用 Redis Push Stream 时启动消费线程，并在停机前先停止流消费。

配置脱敏由 `ServerConfig::ToSafeString()` 处理，启动日志不会输出数据库密码或 Redis 密码。

## 网络层

文件：

- `include/net/TcpServer.h`、`src/TcpServer.cc`
- `include/net/ConnectionContext.h`、`src/ConnectionContext.cc`
- `include/net/TcpConnection.h`、`src/TcpConnection.cc`
- `include/net/IMessageHandler.h`
- `include/net/ResponseTask.h`
- `include/net/ConnectionMeta.h`

职责：

- 使用 epoll 管理监听 socket、客户端连接、eventfd 和 timerfd。
- 为每条连接分配 `ConnectionId`，维护 fd、连接上下文和连接元信息。
- 在 `ConnectionContext` 中维护收包状态与待发送缓冲。
- 使用 `PacketCodec` 从 TCP 字节流中切出完整 JSON 消息。
- 将业务请求投递到 `ThreadPool`，再由 I/O 线程统一写响应和主动推送。
- 在 I/O 线程执行登录态绑定、解绑和投递校验。
- 周期扫描空闲连接与已认证连接心跳超时，关闭超时连接并清理 session。

关键边界：

- Worker 线程不能直接操作 socket。
- `TcpServer` 只依赖 `IMessageHandler` 抽象，不直接依赖具体业务服务。
- `HandleResult` 同时承载响应、session 副作用和主动推送任务。
- 本地多端在线通过 `user_id -> vector<connection_id>` 管理，目标用户同节点多个连接都会收到在线推送。

## 协议与编解码

文件：

- `include/codec/packet_codec.h`、`src/codec/packet_codec.cc`
- `include/codec/json_codec.h`、`src/codec/json_codec.cc`
- `include/protocol/auth_messages.h`
- `include/protocol/chat_messages.h`
- `include/protocol/friend_messages.h`
- `include/protocol/group_messages.h`
- `include/protocol/protocol_helper.h`、`src/protocol/protocol_helper.cc`
- `include/common/message.h`
- `include/common/response.h`
- `include/common/error_code.h`
- `include/common/types.h`

职责：

- `PacketCodec` 负责换行分包和请求大小限制。
- `JsonCodec` 负责信封解析、响应编码和各业务 `data` 结构转换。
- `protocol/*_messages.h` 定义认证、聊天、好友和群组协议 DTO。
- `error_code.h` 定义协议、认证、数据库、消息、好友、群组和内部错误码。

当前主要消息类型：

- 认证：`register`、`login`、`resume_session`、`logout`、`whoami`、`heartbeat`
- 单聊：`send_message`、`pull_offline_messages`、`message_ack`、`mark_message_read`
- 好友：`add_friend`、`accept_friend`、`delete_friend`、`list_friends`
- 群组：`create_group`、`add_group_member`、`send_group_message`
- 推送：`message_push`、`message_sync_push`、`group_message_push`

## Handler 层

文件：

- `include/handler/message_handler.h`
- `src/handler/message_handler.cc`

职责：

- 解析原始 JSON 请求并校验通用信封。
- 维护需要登录的 `msg_type` 白名单。
- 按消息类型调用 User、Chat、Friend、Group service。
- 将业务结果转换成统一响应。
- 构造在线单聊 push、发送方多端同步 push 和群消息 push。
- 对未启用的可选服务返回稳定错误，而不是让请求崩溃。

Handler 只做协议兜底和业务编排，具体业务规则放在 Service 层。

## Service 层

### UserService

文件：

- `include/service/user_service.h`
- `src/service/user_service.cc`

职责：

- 注册、登录、断线恢复、登出、`whoami`。
- 调用 `Validator` 做用户名、密码和 token 校验。
- 使用 `BcryptPasswordHasher` 生成 bcrypt 哈希，并兼容旧 `std::hash` 十进制哈希的登录升级。
- 使用 `getrandom` 生成 64 字符十六进制随机 token。
- 可选调用 `RedisRateLimiter` 做注册和登录限流。
- 可选调用 `RedisSessionStore` 保存 token、presence，登出时撤销 token。

### ChatService

文件：

- `include/service/chat_service.h`
- `src/service/chat_service.cc`

职责：

- 单聊发送、幂等处理、离线消息拉取。
- 消息内容、`client_msg_id`、游标和消息 ID 校验。
- 调用 `MessageRepository` 创建会话、创建消息、查询离线消息。
- 启用 Redis 时使用 `MessageDedupCache` 和 `RedisPushStream` 支持跨节点去重与推送。
- `message_ack` 将消息从 `stored` 推进到 `delivered`。
- `mark_message_read` 将消息推进到 `read`。
- 为同节点接收者多个连接生成推送目标，并向发送者其他本地连接生成 `message_sync_push`。

消息状态是客户端显式确认语义：服务端在线 push 或离线拉取不再隐式标记 `delivered`，需要客户端发送 `message_ack`。

### FriendService

文件：

- `include/service/friend_service.h`
- `src/service/friend_service.cc`

职责：

- 添加好友、同意好友、删除好友、好友列表。
- 防止自己添加自己。
- 区分 pending、accepted、blocked 等关系状态。
- 当前单聊发送策略不强制好友关系。

### GroupService

文件：

- `include/service/group_service.h`
- `src/service/group_service.cc`

职责：

- 创建群组和初始成员。
- 群主或管理员添加成员。
- 校验群成员身份后发送群消息。
- 第一版采用小群 fanout：为除发送者外的每个成员创建一条消息记录；在线成员收到 `group_message_push`，离线成员后续通过离线拉取获取。

## Repository 与数据库层

文件：

- `include/db/db_connection.h`、`src/db/db_connection.cc`
- `include/db/db_pool.h`、`src/db/db_pool.cc`
- `include/db/mysql_statement.h`、`src/db/mysql_statement.cc`
- `include/db/user_repository.h`、`src/db/user_repository.cc`
- `include/db/message_repository.h`、`src/db/message_repository.cc`
- `include/db/friend_repository.h`、`src/db/friend_repository.cc`
- `include/db/group_repository.h`、`src/db/group_repository.cc`
- `sql/001_create_chat_tables.sql`

职责：

- `DbConnection` 封装 MySQL 连接生命周期。
- `DbPool` 管理最小和最大连接数、借用超时、坏连接丢弃和统计。
- `MysqlStatement` 封装 prepared statement 参数绑定。
- 所有生产 Repository 使用 prepared statement 处理用户输入。
- `UserRepository` 支持按用户名和 ID 查询、创建用户、升级密码哈希。
- `MessageRepository` 支持单聊会话创建、消息创建、离线查询、ACK、已读和幂等查询。
- `FriendRepository` 支持好友关系读写。
- `GroupRepository` 支持群组与群成员读写。

Schema 当前包含：

- `users`
- `friendships`
- `conversations`
- `groups`
- `group_members`
- `conversation_members`
- `messages`

`conversations.last_seq` 和 `messages.sequence` 用于会话内单调递增序号。`messages` 通过 `(from_user_id, client_msg_id)` 保证发送幂等，通过 `(conversation_id, sequence)` 保证会话内序号唯一。

## Redis 与缓存层

文件：

- `include/redis/*.h`
- `src/redis/*.cc`
- `include/cache/*.h`
- `src/cache/*.cc`
- `include/server/redis_session_store.h`
- `src/server/redis_session_store.cc`
- `include/stream/redis_push_stream.h`
- `src/stream/redis_push_stream.cc`

职责：

- `RedisConnection`、`RedisClient`、`RedisPool` 封装 Redis 命令和连接池。
- `CachedUserRepository` 缓存用户查询结果，并在 Redis 故障时回源。
- `RedisRateLimiter` 实现注册、登录、发送消息限流。
- `MessageDedupCache` 对发送幂等做 Redis 快速路径。
- `RedisSessionStore` 管理 token、user presence 和 connection presence。
- `RedisPushStream` 基于 Redis Stream 做跨节点推送，支持 consumer group、pending 重试、死信队列和投递去重。

Redis 是可选组件。关闭 Redis 后，单实例登录、在线推送、离线拉取、好友和群聊仍可运行；分布式在线状态、限流、Redis 去重和跨节点推送不会启用。

## 安全与校验

文件：

- `include/security/password_hasher.h`
- `src/security/password_hasher.cc`
- `include/common/validator.h`
- `src/common/validator.cc`

职责：

- `BcryptPasswordHasher` 使用 bcrypt 生成带 salt 的密码哈希。
- 登录时兼容旧十进制 `std::hash`，成功后触发 rehash 升级。
- `Validator` 统一校验用户名、注册密码、登录密码、昵称、消息内容、`client_msg_id`、`message_id`、`conversation_id`、游标和 token。
- token 必须是 64 字符十六进制字符串。

日志和错误响应不输出明文密码、token、数据库密码、Redis 密码或 SQL 原文。

## Session 与多端

文件：

- `include/server/session_manager.h`
- `src/server/session_manager.cc`
- `include/model/connection_session.h`

职责：

- 管理进程内 `connection_id -> session`。
- 管理本地 `user_id -> vector<connection_id>`。
- 同一用户可在本节点保持多个已认证连接。
- `GetConnectionIds(user_id)` 返回该用户所有本地在线连接。
- 清理一个连接不会踢掉同用户其他本地连接。

当前多端策略是本地连接级多端：不引入显式 `device_id`，每条已认证连接视作一个在线端。跨节点 Redis presence 仍是用户级单 presence，完整跨节点多端能力见 [工程路线图](roadmap.md)。

## 日志层

文件：

- `include/common/logger.h`
- `src/common/logger.cc`

职责：

- 提供 `LOG_DEBUG`、`LOG_INFO`、`LOG_WARN`、`LOG_ERROR` 流式宏。
- 输出 UTC 毫秒时间、Linux 线程 ID、级别和模块名。
- 支持控制台和滚动文件。
- 支持异步队列、背压丢弃低级别日志、停机 drain 和 flush。

## 测试结构

测试覆盖范围：

- 编解码：`packet_codec_test`、`json_codec_test`、`connection_context_test`
- 配置和日志：`config_loader_test`、`logger_test`
- 安全：`password_hasher_test`、`validator_test`、`user_service_security_test`
- Repository：`user_repository_test`、`message_repository_test`、`friend_repository_test`、`group_repository_test`
- Service：`user_service_test`、`chat_service_test`、`friend_service_test`、`group_service_test`
- Redis：`redis_*_test`、`chat_service_redis_test`、`cross_server_push_router_test`
- 网络：`server_integration_test`、`auth_integration_test`、`server_timeout_test`、`remote_push_io_test`
- 运行时稳定性：`thread_pool_test`、`server_shutdown_test`、`db_pool_test`

真实 MySQL 和 Redis 集成测试在缺少对应环境变量时会跳过，具体矩阵见 `docs/test.md`。
