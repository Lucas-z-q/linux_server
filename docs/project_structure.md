# 项目结构说明

## 项目概览

本项目是一个基于 C++17 的 TCP 长连接聊天服务器雏形。当前已经实现的业务范围包括用户注册、登录、登出、心跳、基于连接的登录态查询、在线单聊消息路由、消息持久化数据模型、消息仓储和离线消息拉取。整体架构按如下链路分层：

```text
client
  -> TcpServer / ConnectionContext / PacketCodec
  -> IMessageHandler / MessageHandler / JsonCodec
  -> UserService / ChatService / SessionManager
  -> UserRepository / MessageRepository / DbPool / DbConnection / sql schema
  -> MySQL
```

主程序目标是 `server`。它启动一个基于 epoll 的 TCP 服务器，通过工作线程池处理业务请求，使用换行分隔的 JSON 作为应用层协议，由 `MessageHandler` 路由消息，并通过 MySQL 仓储层持久化用户和聊天消息数据。聊天消息当前采用 at-least-once 语义：服务端先将消息保存为 `stored`，再尝试在线 push 或离线拉取返回；在补齐客户端 ACK 或 I/O 投递确认前，服务端不会提前把消息推进为 `delivered`。

## 目录树

```text
.
|-- CMakeLists.txt
|-- README.md
|-- client/
|   `-- client.cc
|-- docs/
|   `-- project_structure.md
|-- include/
|   |-- app/
|   |   `-- main_runner.h
|   |-- codec/
|   |   |-- json_codec.h
|   |   `-- packet_codec.h
|   |-- common/
|   |   |-- error_code.h
|   |   |-- message.h
|   |   |-- response.h
|   |   `-- types.h
|   |-- concurrency/
|   |   `-- thread_pool.h
|   |-- config/
|   |   `-- db_config.h
|   |-- db/
|   |   |-- db_connection.h
|   |   |-- db_connection_factory.h
|   |   |-- db_pool.h
|   |   |-- db_pool_config.h
|   |   `-- user_repository.h
|   |-- handler/
|   |   `-- message_handler.h
|   |-- model/
|   |   |-- connection_session.h
|   |   `-- user_record.h
|   |-- net/
|   |   |-- ConnectionContext.h
|   |   |-- ConnectionMeta.h
|   |   |-- IMessageHandler.h
|   |   |-- ResponseTask.h
|   |   |-- TcpConnection.h
|   |   `-- TcpServer.h
|   |-- protocol/
|   |   |-- auth_messages.h
|   |   |-- chat_messages.h
|   |   `-- protocol_helper.h
|   |-- server/
|   |   |-- connection_manager.h
|   |   `-- session_manager.h
|   |-- service/
|   |   |-- chat_service.h
|   |   `-- user_service.h
|   `-- EchoHandler.h
|-- src/
|   |-- app/
|   |   `-- main_runner.cc
|   |-- codec/
|   |   |-- json_codec.cc
|   |   `-- packet_codec.cc
|   |-- concurrency/
|   |   `-- thread_pool.cc
|   |-- db/
|   |   |-- db_connection.cc
|   |   |-- db_pool.cc
|   |   `-- user_repository.cc
|   |-- handler/
|   |   `-- message_handler.cc
|   |-- protocol/
|   |   `-- protocol_helper.cc
|   |-- server/
|   |   `-- session_manager.cc
|   |-- service/
|   |   |-- chat_service.cc
|   |   `-- user_service.cc
|   |-- ConnectionContext.cc
|   |-- EchoHandler.cc
|   |-- TcpConnection.cc
|   |-- TcpServer.cc
|   |-- main.cc
|   `-- server.cc
|-- sql/
|   `-- 001_create_chat_tables.sql
|-- tests/
|   |-- auth_integration_test.cc
|   |-- db_connection_test.cc
|   |-- db_pool_test.cc
|   |-- fake_auth_server.cc
|   |-- fake_server.cc
|   |-- json_codec_test.cc
|   |-- main_test.cc
|   |-- message_handler_test.cc
|   |-- packet_codec_test.cc
|   |-- server_integration_test.cc
|   |-- server_shutdown_test.cc
|   |-- session_manager_test.cc
|   |-- thread_pool_test.cc
|   |-- user_repository_test.cc
|   |-- user_service_test.cc
|   `-- README_message_handler_cases.md
`-- third_party/
    `-- nlohmann/
        `-- json.hpp
```

`build/`、`Testing/`、`.git/`、`.vscode/` 以及工具元数据目录属于本地生成文件或开发环境文件，不属于源码结构。

## 构建与目标

`CMakeLists.txt` 是项目的构建中心。

- 语言标准：C++17。
- 主服务端目标：`server`。
- 客户端目标：`client`。
- 外部依赖：POSIX threads、MySQL client library、GoogleTest，以及随项目 vendored 的 `nlohmann/json.hpp`。
- 测试目标：覆盖 codec、handler、service、session、thread pool、server shutdown、database connection、database pool、repository、聊天 service 路径和端到端服务器流程。

生产环境的 `server` 目标主要由以下文件组成：

```text
src/main.cc
src/app/main_runner.cc
src/TcpServer.cc
src/ConnectionContext.cc
src/TcpConnection.cc
src/EchoHandler.cc
src/codec/*.cc
src/db/*.cc
src/handler/message_handler.cc
src/protocol/protocol_helper.cc
src/server/session_manager.cc
src/service/chat_service.cc
src/service/user_service.cc
src/concurrency/thread_pool.cc
```

`src/server.cc` 是一个独立的旧版 TCP 示例程序，当前没有被加入 CMake 的 `server` 目标。

## 分层架构

### 入口与依赖装配层

文件：

- `src/main.cc`
- `include/app/main_runner.h`
- `src/app/main_runner.cc`

职责：

- 从环境变量读取数据库配置：`CHAT_DB_HOST`、`CHAT_DB_PORT`、`CHAT_DB_USER`、`CHAT_DB_PASSWORD`、`CHAT_DB_NAME`。
- 初始化 `DbPool`。
- 按 `DbPool -> UserRepository -> SessionManager -> UserService / ChatService -> MessageHandler -> TcpServer` 的顺序装配依赖。
- 通过 `RunMain` 启动 `TcpServer`。

这一层负责应用整体组装。下层模块通过构造函数接收依赖，不自行创建完整依赖图。

### 网络层

文件：

- `include/net/TcpServer.h`、`src/TcpServer.cc`
- `include/net/ConnectionContext.h`、`src/ConnectionContext.cc`
- `include/net/TcpConnection.h`、`src/TcpConnection.cc`
- `include/net/IMessageHandler.h`
- `include/net/ResponseTask.h`
- `include/net/ConnectionMeta.h`

职责：

- 接收 TCP 连接并管理 epoll 事件循环。
- 维护连接 ID 映射和文件描述符映射。
- 在 `ConnectionContext` 中保存单连接的收包解析状态和待发送缓冲区。
- 使用 `PacketCodec` 将连续字节流切分为完整应用层消息。
- 将业务请求提交到 `ThreadPool`。
- 通过 `ResponseTask` 接收 worker 处理结果，在 I/O 线程应用延迟的会话副作用，并把编码后的响应写回客户端。
- 分发 `HandleResult.pushes` 中的主动推送消息，并在投递前校验目标连接仍绑定目标用户。
- 连接关闭时通知业务处理器清理与该连接绑定的登录态。

关键边界：

- `TcpServer` 只依赖 `IMessageHandler` 抽象，不直接依赖具体的 `MessageHandler`。
- `IMessageHandler::handle()` 在 worker 线程执行，不能直接修改全局会话状态。
- 登录态绑定和解绑通过 `HandleResult` 中的 `SessionAction` 表达，随后由 I/O 线程通过 `applyBindSession()` 或 `applyUnbindSession()` 真正执行。
- 在线单聊 push 通过 `HandleResult.pushes` 回到 I/O 线程统一进入连接写队列，worker 线程不直接操作 socket。

### 编解码与协议层

文件：

- `include/codec/packet_codec.h`、`src/codec/packet_codec.cc`
- `include/codec/json_codec.h`、`src/codec/json_codec.cc`
- `include/protocol/auth_messages.h`
- `include/protocol/chat_messages.h`
- `include/protocol/protocol_helper.h`、`src/protocol/protocol_helper.cc`
- `include/common/message.h`
- `include/common/response.h`
- `include/common/error_code.h`
- `include/common/types.h`

职责：

- `PacketCodec` 负责 TCP 流式数据的应用层分包；当前协议使用换行符分隔消息。
- `JsonCodec` 负责在原始 JSON 字符串、`Message` 和 `Response` 之间转换。
- `auth_messages.h` 定义注册、登录、登出和心跳消息的 `data` 字段模型。
- `chat_messages.h` 定义 `send_message`、`message_push` 和 `pull_offline_messages` 的 `data` 字段模型。
- `Message` 和 `Response` 定义统一协议信封。
- `ErrorCode` 集中定义对外可见的状态码。
- `protocol_helper` 存放协议辅助逻辑，供 codec 或 handler 复用。

这一层不应处理 socket、数据库访问或业务流程编排。

### Handler 层

文件：

- `include/handler/message_handler.h`
- `src/handler/message_handler.cc`

职责：

- 作为网络层和业务服务层之间的桥接层。
- 将原始请求解码为 `Message`。
- 按 `msg_type` 路由：`register`、`login`、`logout`、`heartbeat`、`whoami`、`send_message`、`pull_offline_messages`，以及未知消息兜底。
- 通过 `JsonCodec` 解析业务请求数据。
- 调用 `UserService` 或 `ChatService`。
- 构造统一的 `Response`。
- 返回 `HandleResult`，在登录或登出成功时携带延迟执行的会话副作用，在在线单聊成功时携带主动 push。

Handler 本质上是协调器，不直接拥有数据库访问逻辑，也不直接维护会话映射。

### Service 层

文件：

- `include/service/user_service.h`
- `src/service/user_service.cc`
- `include/service/chat_service.h`
- `src/service/chat_service.cc`

职责：

- 实现用户注册、登录、登出和当前登录态查询。
- 校验请求字段。
- 协调 `IUserRepository` 和 `ISessionManager`。
- 将仓储层状态转换为对客户端可见的业务错误码。
- 生成密码哈希和认证 token。
- 登录成功后生成 `ConnectionSession`。
- 实现在线单聊发送的业务校验、消息落库和目标连接查找。
- 为 `send_message` 生成 `message_id`、`conversation_id`、`created_at` 和初始状态字段。
- 通过 `MessageRepository` 查询离线消息并返回给客户端。

当前值得注意的设计：

- 密码哈希当前使用 `std::hash<std::string>`。
- token 当前按 `token_<user_id>` 生成。
- 聊天消息 ID 当前由 `ChatService` 生成，消息、会话和会话成员由 `MessageRepository` 持久化。
- 目标用户离线时，`sendMessage()` 仍会返回成功，消息保持 `stored`，等待在线 push 重试、离线拉取或后续 ACK 机制推进状态。
- 在线 push 和离线拉取当前都不代表客户端已经可靠收到消息；客户端需要按 `message_id` 去重。
- 头文件中已经把这两处标记为后续可抽离的扩展点。

### 会话与服务端状态层

文件：

- `include/server/session_manager.h`
- `src/server/session_manager.cc`
- `include/server/connection_manager.h`

职责：

- `SessionManager` 在内存中维护 `UserId`、`ConnectionId` 和 `ConnectionSession` 之间的映射。
- `ISessionManager` 为 `UserService` 提供可注入接口，便于测试替换。
- `connection_manager.h` 当前是预留占位，用于后续抽离连接查询、在线状态或广播能力。

当前会话存储是进程内内存状态。代码注释中已经把 Redis 或其他外部存储列为未来方向。

### 数据访问层

文件：

- `include/db/db_connection.h`、`src/db/db_connection.cc`
- `include/db/db_connection_factory.h`
- `include/db/db_pool.h`、`src/db/db_pool.cc`
- `include/db/db_pool_config.h`
- `include/db/user_repository.h`、`src/db/user_repository.cc`
- `include/db/message_repository.h`、`src/db/message_repository.cc`
- `include/config/db_config.h`
- `include/model/user_record.h`
- `include/model/message_record.h`
- `sql/001_create_chat_tables.sql`

职责：

- `DbConfig` 描述 MySQL 连接参数。
- `DbConnection` 封装 MySQL C 客户端句柄和连接生命周期。
- `IDbConnectionFactory` 抽象物理连接创建过程，便于测试和连接池定制。
- `DbPool` 负责数据库连接池、借出超时、健康检查、连接失效处理和统计信息。
- `UserRepository` 实现 `IUserRepository`，向 `UserService` 屏蔽 SQL 细节。
- `UserRecord` 映射 `users` 表查询结果。
- `MessageRepository` 实现 `IMessageRepository`，向 `ChatService` 屏蔽消息、会话和会话成员相关 SQL 细节。
- `MessageRecord`、`ConversationRecord` 和 `MessageStatus` 映射聊天持久化模型。
- `001_create_chat_tables.sql` 定义用户、会话、会话成员和消息持久化表结构。

当前仓储层支持：

- 按用户名查询用户。
- 按用户 ID 查询用户。
- 创建用户。
- 创建消息并维护单聊会话和会话成员。
- 按接收方和游标查询 `stored` 状态的离线消息。
- 将消息状态推进为 `delivered` 或 `read`，状态推进本身是幂等的。

SQL 字符串通过 `mysql_real_escape_string` 做转义。查询和插入结果会映射为 `RepositoryStatus`，让 Service 层可以区分未命中、重复键、连接不可用、借出超时和普通查询失败。

当前聊天 schema 支持：

- `conversations`：记录单聊或群聊会话，`type` 通过 CHECK 约束限制为 `single` 或 `group`。
- `conversation_members`：记录会话成员关系，支持按用户查询会话列表。
- `messages`：记录消息正文、发送方、接收方、客户端幂等 ID、消息状态和 Unix 秒级时间戳。
- `UNIQUE(from_user_id, client_msg_id)`：用于后续实现发送幂等。
- `(to_user_id, status, created_at, id)`：用于后续稳定拉取离线消息。

### 并发工具层

文件：

- `include/concurrency/thread_pool.h`
- `src/concurrency/thread_pool.cc`

职责：

- 提供基础工作线程池。
- 接收 callable 任务，并通过 `std::future` 返回结果。
- 被 `TcpServer` 用于把业务请求处理从 epoll I/O 线程中移出。

### 客户端与示例代码

文件：

- `client/client.cc`
- `include/EchoHandler.h`
- `src/EchoHandler.cc`
- `src/server.cc`

职责：

- `client/client.cc` 是一个简单客户端目标，用于与当前分包协议交互。
- `EchoHandler` 是早期的回显示例处理器，可用于验证网络收发链路。
- `src/server.cc` 是一个独立的阻塞式 TCP 示例，不属于当前主业务路径。

## 核心运行流程

### 启动流程

```text
main()
  -> LoadDbConfigFromEnv()
  -> DbPool::init()
  -> UserRepository(db_pool)
  -> SessionManager()
  -> UserService(user_repository, session_manager)
  -> ChatService(session_manager)
  -> MessageHandler(user_service, chat_service)
  -> TcpServer("127.0.0.1", 8080, handler)
  -> RunMain([server.start])
```

### 请求处理流程

```text
socket readable event
  -> TcpServer::onReadable()
  -> ConnectionContext::feedPacketData()
  -> PacketCodec::feed()
  -> TcpServer::submitRequestTask()
  -> ThreadPool worker
  -> IMessageHandler::handle()
  -> MessageHandler::handle()
  -> JsonCodec::decodeMessage()
  -> route by msg_type
  -> UserService / ChatService
  -> optional UserRepository / SessionManager calls
  -> JsonCodec::encodeResponse()
  -> ResponseTask queued for I/O thread
  -> TcpServer::onWorkerResultReadable()
  -> optional applyBindSession / applyUnbindSession
  -> PacketCodec::encode()
  -> pending send buffer
  -> socket writable event
```

### 在线单聊流程

```text
MessageHandler::handleSendMessage()
  -> JsonCodec::parseSendMessageRequest()
  -> ChatService::sendMessage()
  -> ISessionManager::GetSession(from_conn_id)
  -> IMessageRepository::createMessage()
  -> ISessionManager::GetConnectionId(to_user_id)
  -> return SendMessageResult
  -> MessageHandler builds send_message_resp
  -> MessageHandler builds message_push in HandleResult.pushes
  -> TcpServer::onWorkerResultReadable()
  -> validate target connection still belongs to target user
  -> append push payload to target connection pending send buffer
```

### 离线消息拉取流程

```text
MessageHandler::handlePullOfflineMessages()
  -> JsonCodec::parsePullOfflineMessagesRequest()
  -> ChatService::pullOfflineMessages()
  -> ISessionManager::GetSession(conn_id)
  -> IMessageRepository::listOfflineMessages()
  -> return PullOfflineMessagesResult
  -> MessageHandler builds pull_offline_messages_resp
```

这一流程当前只查询 `stored` 状态消息并返回，不在响应写回客户端前推进 `delivered`。因此离线消息拉取也是 at-least-once 语义：如果客户端已经通过在线 push 收到同一条消息，或者拉取响应写回过程中出现重试，客户端需要按 `message_id` 去重。后续补齐 `message_ack` 或 I/O 投递确认路径后，再由确认路径推进 `delivered` 和 `read`。

### 登录流程

```text
MessageHandler::handleLogin()
  -> JsonCodec::parseLoginRequest()
  -> UserService::login()
  -> IUserRepository::findByUsername()
  -> UserRepository::findByUsername()
  -> DbPool::borrow()
  -> MySQL query
  -> password hash comparison
  -> create ConnectionSession
  -> return LoginResult
  -> HandleResult with SessionAction::BIND
  -> I/O thread applies MessageHandler::applyBindSession()
  -> UserService::bindSession()
  -> SessionManager::BindSession()
```

### 登出与连接关闭流程

```text
logout message
  -> MessageHandler::handleLogout()
  -> UserService::logout()
  -> HandleResult with SessionAction::UNBIND
  -> I/O thread applies MessageHandler::applyUnbindSession()
  -> UserService::clearSession()
  -> SessionManager::ClearSession()

connection closed
  -> TcpServer closes and unregisters connection
  -> IMessageHandler::onConnectionClosed()
  -> MessageHandler::onConnectionClosed()
  -> UserService::clearSession()
```

## 依赖关系

高层编译期依赖方向如下：

```text
app/main
  -> net
  -> handler
  -> service
  -> db

handler
  -> codec
  -> protocol
  -> common

service
  -> db interface
  -> session interface
  -> protocol data models
  -> common types and errors

db
  -> config
  -> model
  -> MySQL C client

net
  -> codec/packet_codec
  -> concurrency/thread_pool
  -> common types
  -> IMessageHandler abstraction
```

最重要的依赖倒置点是 `IMessageHandler`：网络服务器只知道处理器接口，不知道具体业务实现。第二个关键依赖倒置点是 `IUserRepository` 和 `ISessionManager`：`UserService` 可以在不依赖真实数据库或真实会话存储的情况下测试。

## 模块清单

| 模块 | 主要文件 | 作用 |
| --- | --- | --- |
| 应用装配 | `src/main.cc`、`src/app/main_runner.cc` | 读取配置、组装依赖、启动服务器 |
| 网络运行时 | `TcpServer`、`ConnectionContext`、`ResponseTask`、`IMessageHandler` | epoll 循环、连接生命周期、请求投递、响应写回 |
| 协议分包 | `PacketCodec` | 将 TCP 字节流切分为换行分隔的应用包 |
| JSON 协议 | `JsonCodec`、`Message`、`Response`、`auth_messages`、`chat_messages` | 在原始 JSON 和类型化协议对象之间转换 |
| 业务路由 | `MessageHandler` | 解码、路由、调用 Service、编码响应 |
| 用户业务逻辑 | `UserService` | 注册、登录、登出、whoami、校验、token 和 session 准备 |
| 聊天业务逻辑 | `ChatService` | 在线单聊校验、消息落库编排、离线消息拉取 |
| 会话状态 | `SessionManager`、`ConnectionSession` | 维护用户到连接、连接到会话的内存映射 |
| 数据库基础设施 | `DbConnection`、`DbPool`、`DbConnectionFactory` | MySQL 连接生命周期与连接池 |
| 用户持久化 | `UserRepository`、`UserRecord` | `users` 表读写操作 |
| 消息持久化 | `MessageRepository`、`MessageRecord`、`ConversationRecord` | `messages`、`conversations` 和 `conversation_members` 表读写操作 |
| 聊天 schema | `sql/001_create_chat_tables.sql` | 会话、会话成员、消息持久化表结构 |
| 并发工具 | `ThreadPool` | 业务请求的 worker 执行 |
| 测试支持 | `tests/fake_server.cc`、`tests/fake_auth_server.cc` | 集成测试用轻量服务端目标 |
| 遗留与示例 | `EchoHandler`、`src/server.cc` | 早期回显和阻塞式 TCP 示例，不是当前核心业务路径 |

## 测试结构

测试按模块组织：

- `packet_codec_test.cc`：分包与封包行为。
- `json_codec_test.cc`：JSON 信封以及认证数据解析和编码。
- `message_handler_test.cc`：消息路由、响应和聊天 push 生成行为。
- `user_service_test.cc`：使用注入依赖测试用户业务流程。
- `chat_service_test.cc`：在线单聊 service 校验、目标在线查找、消息落库编排和离线拉取行为。
- `session_manager_test.cc`：内存会话映射行为。
- `thread_pool_test.cc`：工作线程池行为。
- `db_connection_test.cc`：MySQL 连接封装。
- `db_pool_test.cc`：连接池生命周期和借出行为。
- `user_repository_test.cc`：用户仓储层数据访问。
- `message_record_test.cc`：消息状态枚举、状态转换和消息模型构造。
- `server_shutdown_test.cc`：服务器停止与关闭行为。
- `server_integration_test.cc`：TCP 服务端集成路径。
- `auth_integration_test.cc`：认证相关集成路径。
- `main_test.cc`：`RunMain` 行为。

集成测试会通过 CMake 构建小型测试服务端：

- `server_test_server` 来自 `tests/fake_server.cc`。
- `auth_test_server` 来自 `tests/fake_auth_server.cc`。

## 当前扩展点

代码中已经预留了比较清晰的扩展方向：

- 随着消息类型增长，将 `MessageHandler` 拆分为认证、聊天、好友、群组等子处理器。
- 新增 `message_ack` 或接入 I/O 投递确认路径，由确认结果推进 `delivered`。
- 新增已读上报协议，由客户端 ACK 推进 `read`。
- 将 `std::hash` 密码哈希替换为独立的密码哈希组件。
- 将确定性的 token 生成替换为安全 token 生成器。
- 将连接查询和广播能力正式抽离到 `ConnectionManager`。
- 如果需要多实例或进程重启后保留登录态，引入 Redis 或其他外部会话存储。
- 将 `PacketCodec` 从换行分隔协议升级为长度前缀协议。
- 在认证协议之外扩展聊天、离线消息、好友和群组协议模型。
- 为 `MessageRepository` 增加真实 MySQL 集成测试，覆盖事务、唯一键冲突、游标 SQL 和状态幂等。

## 当前边界说明

- `src/server.cc` 和 `EchoHandler` 属于示例或遗留代码，有助于理解项目演进，但不是当前主业务路径。
- `include/server/connection_manager.h` 当前只是占位文件，没有实际功能实现。
- 消息投递当前是 at-least-once 语义；在线 push 和离线拉取都不会在响应真正到达客户端前推进 `delivered`。
- 客户端必须按 `message_id` 做幂等去重。
- `delivered` 和 `read` 依赖后续 `message_ack`、已读上报或 I/O 投递确认路径推进。
- `README.md` 当前更偏向外部协议说明，而不是代码组织说明。
- `AGENTS.md`、`CLAUDE.md`、`.codegraph/` 属于本地代理或工具上下文，不属于源码结构。
- `third_party/nlohmann/json.hpp` 是随项目 vendored 的第三方 JSON 头文件，通过 `third_party` include 路径使用。
- `build/` 下的生成文件不应被视为源码模块。
