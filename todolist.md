# Linux C++ 聊天服务器后续开发 Todo List

本文档记录聊天服务器从“认证服务器 + MySQL 连接池”演进到“可演示的长连接聊天系统”的任务拆解和当前完成状态。

当前基础能力：

- 已具备 `epoll + 非阻塞 socket + worker 线程池` 网络框架。
- 已具备 JSON 协议、packet codec、基础请求响应模型。
- 已实现用户注册、登录、断线恢复、登出、心跳、`whoami`。
- 已实现进程内 session 管理和本地多端在线连接管理。
- 已实现 MySQL 用户、消息、好友和群组 Repository。
- 已实现生产代码 prepared statement，用户输入不再通过 SQL 字符串拼接进入查询。
- 已实现 bcrypt 密码哈希、旧哈希兼容升级、随机 token、Redis token 存储和撤销。
- 已实现统一输入验证模块 `Validator`。
- 已实现 `send_message` 在线单聊消息转发、发送方多端同步和 `message_push` 主动推送。
- 已实现离线消息持久化、拉取、客户端 `message_ack` 和 `mark_message_read`。
- 已实现 conversation 内单调递增消息序号。
- 已接入 Redis 连接池、在线状态、用户缓存、消息去重、限流和跨节点消息路由。
- 已实现好友申请、同意、删除和好友列表。
- 已实现小群创建、成员添加和群消息 fanout。
- 已实现统一配置加载、脱敏输出、异步日志、连接空闲超时和心跳超时回收。

## 阶段总览

| 阶段 | 模块 | 优先级 | 状态 |
| --- | --- | --- | --- |
| 1 | 消息转发核心业务实现 | P0 | **已完成** |
| 2 | 离线消息与消息持久化系统 | P0 | **已完成** |
| 3 | Redis 集成与分布式支持 | P1 | **已完成** |
| 4 | 配置系统完善 | P1 | **已完成** |
| 5 | 日志系统实现 | P1 | **已完成** |
| 6 | 连接管理与超时回收 | P1 | **已完成** |
| 7 | 系统安全性增强 | P1 | **已完成** |
| 8 | 聊天业务模型完善 | P2 | **已完成（第一版）** |

优先级说明：

- P0：形成聊天服务器最小可演示闭环必须完成。
- P1：让项目具备生产化亮点和稳定性必须完成。
- P2：扩展聊天产品能力，可在核心闭环后逐步实现。

## 1. 消息转发核心业务实现

### 1.1 协议与数据结构

- [x] 定义 `send_message` 请求结构。
  - 状态：`SendMessageRequest` 包含 `to_user_id`、`content`、`client_msg_id`。

- [x] 定义服务端主动推送事件结构。
  - 状态：`MessagePushData` 包含消息 ID、会话 ID、会话内 `sequence`、发送方、接收方、内容和服务端时间。

### 1.2 在线用户查找

- [x] 扩展 SessionManager 支持 `user_id -> connection_id` 查询。
  - 状态：`GetConnectionId(UserId)` 保留兼容路径。

- [x] 明确多端登录第一版策略。
  - 状态：当前采用本地连接级多端策略，`GetConnectionIds(UserId)` 返回同一用户在本节点的所有已认证连接。

### 1.3 主动推送链路

- [x] 为 TcpServer 增加按 `connection_id` 主动推送接口。
  - 状态：`TcpServer::deliverRemotePush()` 支持跨线程安全投递，worker 线程不直接操作 socket。

- [x] 将 MessageHandler 的返回模型扩展为支持“响应 + 额外推送”。
  - 状态：`HandleResult` 包含 push 任务、session 副作用和消息投递信息。

### 1.4 send_message 业务逻辑

- [x] 实现 `send_message` 完整处理流程。
  - 状态：`MessageHandler::handleSendMessage()` 和 `ChatService::sendMessage()` 完成登录校验、参数验证、幂等、持久化、在线推送、离线保留、Redis 路由和发送方多端同步。

### 1.5 错误处理策略

- [x] 建立消息转发错误码。
  - 状态：`error_code.h` 包含消息、限流、幂等、好友和群组错误码。

## 2. 离线消息与消息持久化系统

### 2.1 MySQL 表结构

- [x] 设计 `conversations` 表。
  - 状态：支持 `single`、`group` 类型，`single_chat_key` 唯一约束，`last_seq` 分配会话内序号。

- [x] 设计 `messages` 表。
  - 状态：包含 `sequence`、幂等唯一索引、离线查询索引和会话序号唯一索引。

### 2.2 Repository 与 Service

- [x] 新增 `MessageRepository`。
  - 状态：支持 `createMessage()`、`listOfflineMessages()`、`markDelivered()`、`markRead()`、`findOrCreateSingleConversation()`、`findMessageByClientMsgId()`。

- [x] 新增 `ChatService`。
  - 状态：支持发送单聊、拉取离线、ACK、已读、跨节点 push 发布和本地多端同步。

### 2.3 离线写入与拉取

- [x] 实现目标用户离线时自动存储消息。
  - 状态：离线目标仍成功入库，发送方收到 `send_message_resp`。

- [x] 实现 `pull_offline_messages`。
  - 状态：支持 `limit` 和 `since_message_id` 游标分页，返回 `sequence` 和 `has_more`。

### 2.4 消息状态管理

- [x] 实现消息投递状态更新。
  - 状态：客户端发送 `message_ack` 后服务端调用 `markDelivered()`，不再把在线 push 或离线拉取入队等同于客户端确认。

- [x] 实现已读回执接口。
  - 状态：`mark_message_read` 支持单个 `message_id` 或批量 `message_ids`，成功后调用 `markRead()`。

## 3. Redis 集成与分布式支持

### 3.1 Redis 客户端与连接池

- [x] 选择并接入 Redis C/C++ 客户端。
  - 状态：使用 `hiredis`，封装 `RedisConnection`、`RedisClient`，CMake 支持系统依赖或 FetchContent。

- [x] 实现 Redis 连接池。
  - 状态：`RedisPool` 支持最小/最大连接数、借用超时、坏连接丢弃、统计信息和优雅停机。

### 3.2 在线状态与 session 迁移

- [x] 设计 Redis 在线状态 key。
  - 状态：`RedisSessionStore` 管理 token key、user presence key 和 connection presence key，支持 TTL 和心跳刷新。

- [x] 将 session 存储抽象为接口。
  - 状态：`IGlobalSessionStore` 抽象全局 token 和 presence，本地 `SessionManager` 管理进程内连接状态。

### 3.3 分布式消息路由

- [x] 设计多节点在线路由模型。
  - 状态：每个节点配置 `server_id`，同节点直接推送，跨节点通过 Redis Stream。

- [x] 接入 Redis Stream 作为跨节点投递通道。
  - 状态：`RedisPushStream` 支持 Stream、consumer group、pending 重试、死信队列和投递去重。

### 3.4 缓存策略

- [x] 缓存用户基础信息。
  - 状态：`CachedUserRepository` 缓存 `findById` 和 `findByUsername`，Redis 故障时回源。

## 4. 配置系统完善

- [x] 选择并定义配置文件格式。
  - 状态：使用 JSON 配置，包含 server、mysql、redis、log、timeout、connection、heartbeat 配置段。

- [x] 实现 `ConfigLoader`。
  - 状态：支持 `--config`、`CHAT_CONFIG_PATH`、环境变量覆盖、类型校验和范围校验。

- [x] 将硬编码配置迁移到配置文件。
  - 状态：监听地址、MySQL、Redis、日志、连接和心跳超时均由 `ServerConfig` 驱动。

- [x] 实现配置脱敏输出。
  - 状态：启动日志通过 `ServerConfig::ToSafeString()` 隐藏敏感字段。

## 5. 日志系统实现

- [x] 实现轻量级 Logger。
  - 状态：支持 `DEBUG`、`INFO`、`WARN`、`ERROR`，每条日志包含时间戳、线程 ID、级别和模块名。

- [x] 替换现有标准输出。
  - 状态：项目代码统一使用 `LOG_*` 宏。

- [x] 支持日志文件输出和滚动。
  - 状态：支持按大小滚动和保留最近 N 个日志文件。

- [x] 支持异步日志写入。
  - 状态：后台线程异步写入，停机时 drain 队列并 flush。

## 6. 连接管理与超时回收

- [x] 补齐连接元信息。
  - 状态：`ConnectionMeta` 包含连接 ID、fd、peer、收发时间、认证用户、统计计数和状态。

- [x] 实现定时扫描机制。
  - 状态：`TcpServer` 使用 timerfd 周期唤醒 I/O 线程并有界扫描连接。

- [x] 实现心跳超时踢下线。
  - 状态：已认证连接超过 heartbeat timeout 后自动断线并清理本地 session。

- [x] 断线流程同步 Redis。
  - 状态：`RedisSessionStore::ClearPresence()` 校验 `connection_id` 和 token 后删除 presence，避免误删新连接状态。

## 7. 系统安全性增强

### 7.1 密码安全

- [x] 替换 `std::hash` 密码哈希。
  - 状态：新增 `IPasswordHasher` 和 `BcryptPasswordHasher`，注册保存 bcrypt 哈希；登录兼容旧十进制哈希并在成功登录后升级为 bcrypt。

### 7.2 Token 安全

- [x] 重构 token 生成机制。
  - 状态：`UserService::generateToken()` 使用 `getrandom` 生成 64 字符十六进制随机 token。

- [x] 增加重放保护策略。
  - 状态：Redis 保存有效 token，登出和重复登录会撤销旧 token；`resume_session` 和带 token 的受保护请求会校验 Redis token 状态。

### 7.3 SQL 安全

- [x] 将用户 Repository 改为 prepared statement。
  - 状态：`UserRepository` 的查找、创建和密码哈希更新均使用 `MysqlStatement` 绑定参数。

- [x] 将消息 Repository 使用 prepared statement。
  - 状态：`MessageRepository` 创建会话、创建消息、查询离线、ACK、已读和幂等查询均使用绑定参数。

### 7.4 输入验证

- [x] 建立统一输入验证模块。
  - 状态：`Validator` 覆盖 username、password、nickname、message content、client message id、message id、conversation id、cursor 和 token，Service 层统一调用。

## 8. 聊天业务模型完善

### 8.1 好友关系

- [x] 设计好友表。
  - 状态：`friendships` 表支持 pending、accepted、blocked，并通过无向唯一索引防重复。

- [x] 实现好友接口。
  - 状态：已新增 `FriendRepository`、`FriendService` 和 `add_friend`、`accept_friend`、`delete_friend`、`list_friends` 协议分支。

### 8.2 单聊会话

- [x] 完善单聊 conversation 管理。
  - 状态：`findOrCreateSingleConversation()` 自动创建或复用同一对用户的单聊 conversation。

### 8.3 群聊

- [x] 设计群组和成员表。
  - 状态：`groups`、`group_members` 和 `conversation_members` 已定义，群角色支持 owner、admin、member。

- [x] 实现群消息群发。
  - 状态：`create_group`、`add_group_member`、`send_group_message` 已实现；第一版按小群 fanout 为每个接收成员创建消息，在线成员收到 `group_message_push`，离线成员后续可拉取。

### 8.4 消息可靠性

- [x] 设计消息 ACK 机制。
  - 状态：`message_ack` 支持单条和批量 ACK，成功后消息状态推进为 `delivered`。

- [x] 实现消息去重。
  - 状态：`MessageDedupCache` 和 `(from_user_id, client_msg_id)` 唯一索引共同保证幂等。

- [x] 实现消息序号。
  - 状态：`conversations.last_seq` 在事务中分配，`messages.sequence` 在同一 conversation 内单调递增且唯一。

### 8.5 多端登录

- [x] 制定多端登录策略。
  - 状态：第一版采用本地连接级多端策略，同一用户可在同一节点绑定多个连接；不引入显式 `device_id`。

- [x] 实现多端消息同步。
  - 状态：单聊发送后，接收者同节点所有在线连接收到 `message_push`，发送者其他同节点连接收到 `message_sync_push`。

## 推荐实施路线完成情况

### 第 1 周：安全加固

- [x] 引入 bcrypt 替换简单密码哈希。
- [x] 封装 MySQL prepared statement 并改造 UserRepository 和 MessageRepository。
- [x] 建立统一输入验证模块。
- [x] 实现已读回执协议接口。

### 第 2 周：消息可靠性增强

- [x] 实现客户端消息 ACK 协议。
- [x] 实现 conversation 内单调递增消息序号。
- [x] 实现 token 撤销和重放保护策略。

### 第 3 周：好友关系与群聊基础

- [x] 设计并实现好友表和好友接口。
- [x] 设计群组表并实现群消息群发。

### 第 4 周：多端登录与产品打磨

- [x] 制定并实现本地多端登录策略。
- [x] 实现本地多端消息同步。
- [x] 端到端集成测试覆盖核心路径。

## 全局验收标准

- [x] `cmake --build build` 通过。
- [x] `ctest --test-dir build --output-on-failure` 通过。
- [x] 两个客户端可完成注册、登录、在线单聊、登出。
- [x] 目标用户离线时消息写入 MySQL，重新登录后可拉取。
- [x] Redis 中能看到在线状态和 session 信息。
- [x] 服务端日志有统一格式，关键错误可排查。
- [x] 空闲连接和心跳超时连接能自动清理。
- [x] 密码、token、数据库密码和 Redis 密码不会以明文形式出现在日志中。
- [x] 主要业务路径有单元测试或集成测试覆盖。

## 后续增强项

- 跨节点多端 presence 可从用户级扩展为 `user_id -> device_id -> connection`。
- 消息状态当前是消息级 ACK，后续可扩展为按设备维度的 ACK/read receipt。
- 群聊当前适合小群 fanout，后续可增加大群分片、异步投递任务和历史消息查询接口。
- 协议当前以换行分包，后续可升级为长度前缀协议。
