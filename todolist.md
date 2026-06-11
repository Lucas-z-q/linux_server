# Linux C++ 聊天服务器后续开发 Todo List

本文档用于系统规划聊天服务器从"认证服务器 + MySQL 连接池"继续演进为"可演示的高并发聊天系统"的开发任务。

当前基础能力：

- 已具备 `epoll + 非阻塞 socket + worker 线程池` 网络框架。
- 已具备 JSON 协议、packet codec、基础请求响应模型。
- 已实现用户注册、登录、登出、心跳、`whoami`。
- 已实现进程内 session 管理。
- 已实现 MySQL 用户 Repository 和第一版生产级 MySQL 连接池。
- 已补充连接池结构化错误模型、日志统计和 fake-driven 单元测试。
- 已实现 `send_message` 在线单聊消息转发和 `message_push` 主动推送。
- 已实现离线消息持久化、拉取和投递状态更新。
- 已接入 Redis 连接池、在线状态管理、跨节点消息路由（Redis Stream）。
- 已实现统一配置加载（JSON + 环境变量覆盖 + 校验 + 脱敏输出）。
- 已实现异步日志系统（级别控制、文件滚动、后台线程写入）。
- 已实现连接空闲超时和心跳超时的自动回收。
- 已实现 Redis 用户缓存、消息去重缓存和登录/注册/发送限流。

后续核心目标：

- 完善安全机制：密码哈希、token 签名、prepared statement、统一输入验证。
- 支持好友关系和群聊业务模型。
- 实现消息 ACK、已读回执和消息序号。
- 支持多端登录策略。

## 阶段总览

| 阶段 | 模块 | 优先级 | 预估工时 | 依赖 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 1 | 消息转发核心业务实现 | P0 | 3-5 天 | 现有网络层、SessionManager、MessageHandler | **已完成** |
| 2 | 离线消息与消息持久化系统 | P0 | 4-6 天 | 阶段 1、MySQL 连接池 | **基本完成** |
| 3 | Redis 集成与分布式支持 | P1 | 5-8 天 | 阶段 1、配置系统基础 | **基本完成** |
| 4 | 配置系统完善 | P1 | 2-3 天 | 现有启动入口、DB 配置 | **已完成** |
| 5 | 日志系统实现 | P1 | 3-5 天 | 配置系统基础 | **已完成** |
| 6 | 连接管理与超时回收 | P1 | 3-5 天 | 网络层连接元信息、日志系统 | **已完成** |
| 7 | 系统安全性增强 | P1 | 5-8 天 | 用户认证流程、MySQL Repository | 进行中 |
| 8 | 聊天业务模型完善 | P2 | 10-20 天 | 阶段 1-3 | 部分完成 |

优先级说明：

- P0：形成聊天服务器最小可演示闭环必须完成。
- P1：让项目具备生产化亮点和稳定性必须完成。
- P2：扩展聊天产品能力，可在核心闭环后逐步实现。

## 1. 消息转发核心业务实现（已完成）

### 1.1 协议与数据结构

- [x] 定义 `send_message` 请求结构。
  - 优先级：P0
  - 状态：**已完成** — `protocol/chat_messages.h` 中定义了 `SendMessageRequest`，包含 `to_user_id`、`content`、`client_msg_id` 字段，`content` 限制 4096 字节。

- [x] 定义服务端主动推送事件结构。
  - 优先级：P0
  - 状态：**已完成** — `MessagePushData` 包含 `message_id`、`conversation_id`、`from_user_id`、`from_username`、`to_user_id`、`content`、`created_at`、`server_time`。

### 1.2 在线用户查找

- [x] 扩展 SessionManager 支持 `user_id -> connection_id` 查询。
  - 优先级：P0
  - 状态：**已完成** — `ISessionManager::GetConnectionId(UserId)` 和 `SessionManager` 实现，使用 `user_to_connection_` 映射。

- [x] 明确多端登录第一版策略。
  - 优先级：P0
  - 状态：**已完成** — 采用"同一用户只允许一个在线连接"策略，`BindSession` 覆盖旧连接。

### 1.3 主动推送链路

- [x] 为 TcpServer 增加按 `connection_id` 主动推送接口。
  - 优先级：P0
  - 状态：**已完成** — `TcpServer::deliverRemotePush()` 支持跨线程安全投递，worker 线程不直接操作 socket。`ResponseTask` 支持 push_tasks。

- [x] 将 MessageHandler 的返回模型扩展为支持"响应 + 额外推送"。
  - 优先级：P0
  - 状态：**已完成** — `HandleResult` 包含推送任务，TcpServer 处理 worker 结果时先写响应再处理推送，`onMessagesDelivered` 回调标记已投递。

### 1.4 send_message 业务逻辑

- [x] 实现 `send_message` 完整处理流程。
  - 优先级：P0
  - 状态：**已完成** — `MessageHandler::handleSendMessage()` + `ChatService::sendMessage()` 实现完整流程：登录校验、参数验证、在线推送/离线存储、限流和去重。

### 1.5 错误处理策略

- [x] 建立消息转发错误码。
  - 优先级：P0
  - 状态：**已完成** — `error_code.h` 包含 `MESSAGE_TOO_LONG`、`USER_NOT_ONLINE`、`CANNOT_SEND_TO_SELF`、`NOT_LOGGED_IN`、`IDEMPOTENCY_CONFLICT`、`RATE_LIMITED` 等。

## 2. 离线消息与消息持久化系统（基本完成）

### 2.1 MySQL 表结构

- [x] 设计 `conversations` 表。
  - 优先级：P0
  - 状态：**已完成** — `sql/001_create_chat_tables.sql` 中定义了 `conversations` 表，支持 `single`/`group` 类型，`single_chat_key` 唯一约束。

- [x] 设计 `messages` 表。
  - 优先级：P0
  - 状态：**已完成** — 包含 `id`、`conversation_id`、`client_msg_id`、`from_user_id`、`to_user_id`、`content`、`status`、`created_at`、`delivered_at`、`read_at`，含幂等唯一索引和离线消息查询索引。

### 2.2 Repository 与 Service

- [x] 新增 `MessageRepository`。
  - 优先级：P0
  - 状态：**已完成** — `IMessageRepository` 接口和 `MessageRepository` 实现，支持 `createMessage()`、`listOfflineMessages()`、`markDelivered()`、`markRead()`、`findOrCreateSingleConversation()`、`findMessageByClientMsgId()`。

- [x] 新增 `ChatService`。
  - 优先级：P0
  - 状态：**已完成** — `ChatService` 包含 `sendMessage()`、`pullOfflineMessages()`、`markMessagesDelivered()`、`publishRemotePush()`。

### 2.3 离线写入与拉取

- [x] 实现目标用户离线时自动存储消息。
  - 优先级：P0
  - 状态：**已完成** — `ChatService::sendMessage()` 在目标不在线时写入 MySQL，返回发送方成功响应。

- [x] 实现 `pull_offline_messages`。
  - 优先级：P0
  - 状态：**已完成** — `MessageHandler::handlePullOfflineMessages()` + `ChatService::pullOfflineMessages()` 支持 `limit` 和游标分页。

### 2.4 消息状态管理

- [x] 实现消息投递状态更新。
  - 优先级：P1
  - 状态：**已完成** — 推送成功后通过 `onMessagesDelivered` 回调批量标记 `delivered`，Redis Stream 消费端也回调 `markDelivered`。

- [ ] 实现已读回执接口。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：消息状态管理
  - 状态：**Repository 层已就绪**（`markRead()` 已实现），需在 MessageHandler 中新增 `mark_message_read` 协议分支。
  - 技术要求：
    - 请求字段包含 `message_id` 或批量 `message_ids`。
  - 实施步骤：
    - 新增 `mark_message_read` 协议。
    - 在 MessageHandler 增加分支调用 `markRead()`。
  - 验收标准：
    - 发送方后续查询能看到消息已读状态。

## 3. Redis 集成与分布式支持（基本完成）

### 3.1 Redis 客户端与连接池

- [x] 选择并接入 Redis C/C++ 客户端。
  - 优先级：P1
  - 状态：**已完成** — 使用 `hiredis`，封装了 `RedisConnection`、`RedisClient`，CMake 支持系统安装或 FetchContent 构建。

- [x] 实现 Redis 连接池。
  - 优先级：P1
  - 状态：**已完成** — `RedisPool` 支持最小/最大连接数、借用超时、坏连接丢弃、统计信息和优雅停机。

### 3.2 在线状态与 session 迁移

- [x] 设计 Redis 在线状态 key。
  - 优先级：P1
  - 状态：**已完成** — `RedisSessionStore` 管理 token key、user presence key 和 connection presence key，支持 TTL 和心跳刷新。

- [x] 将 session 存储抽象为接口。
  - 优先级：P1
  - 状态：**已完成** — `IGlobalSessionStore` 接口，本地 `SessionManager` 用于单进程，`RedisSessionStore` 用于分布式部署。

### 3.3 分布式消息路由

- [x] 设计多节点在线路由模型。
  - 优先级：P1
  - 状态：**已完成** — 每个服务节点配置 `server_id`，在线状态记录目标用户所在 `server_id`，同节点直接推送，跨节点通过 Redis Stream。

- [x] 接入 Redis Stream 作为跨节点投递通道。
  - 优先级：P1
  - 状态：**已完成** — `RedisPushStream` 实现 Stream + Consumer Group，支持 pending 重试、死信队列、投递去重和 markDelivered 回调。

### 3.4 缓存策略

- [x] 缓存用户基础信息。
  - 优先级：P2
  - 状态：**已完成** — `CachedUserRepository` 装饰器模式，缓存 `findById`/`findByUsername` 结果，支持 TTL 和 not-found 短缓存，Redis 故障时回源。

## 4. 配置系统完善（已完成）

### 4.1 配置文件格式

- [x] 选择并定义配置文件格式。
  - 优先级：P1
  - 状态：**已完成** — 使用 JSON 格式（`config/server.json`），定义 server、mysql、redis、log、timeout、connection、heartbeat 配置段。

### 4.2 配置加载与校验

- [x] 实现 `ConfigLoader`。
  - 优先级：P1
  - 状态：**已完成** — 支持文件路径参数（`--config`）、环境变量覆盖敏感字段、全量校验、`ConfigResult` variant 返回。

- [x] 将硬编码配置迁移到配置文件。
  - 优先级：P1
  - 状态：**已完成** — 服务器监听 IP/端口、MySQL、Redis、日志、超时均来自 `ServerConfig`，`main.cc` 统一由配置驱动。

### 4.3 敏感信息脱敏

- [x] 实现配置脱敏输出。
  - 优先级：P1
  - 状态：**已完成** — `ServerConfig::ToSafeString()` 将密码字段替换为 `<redacted>`，启动日志只打印脱敏配置。

## 5. 日志系统实现（已完成）

### 5.1 Logger 基础能力

- [x] 实现轻量级 Logger。
  - 优先级：P1
  - 状态：**已完成** — `Logger` 单例支持 `DEBUG`/`INFO`/`WARN`/`ERROR` 级别，每条日志包含时间戳、线程 ID、级别、模块名。宏接口 `LOG_DEBUG/INFO/WARN/ERROR`。

- [x] 替换现有标准输出。
  - 优先级：P1
  - 状态：**已完成** — 项目代码统一使用 `LOG_*` 宏。

### 5.2 文件输出与异步日志

- [x] 支持日志文件输出和滚动。
  - 优先级：P1
  - 状态：**已完成** — 支持按大小滚动、保留最近 N 个日志文件，配置 `log.file_path`、`max_size_mb`、`max_files`。

- [x] 支持异步日志写入。
  - 优先级：P2
  - 状态：**已完成** — 后台 worker 线程异步写入，队列容量可配，停机时 drain 队列并 flush。

## 6. 连接管理与超时回收（已完成）

### 6.1 连接元信息完善

- [x] 补齐连接元信息。
  - 优先级：P1
  - 状态：**已完成** — `ConnectionMeta` 包含 `conn_id`、`fd`、`peer_ip`、`peer_port`、`connected_at`、`last_recv_at`、`last_send_at`、`last_active_at`、`authenticated_user_id`、收发计数和状态。

### 6.2 空闲连接回收

- [x] 实现定时扫描机制。
  - 优先级：P1
  - 状态：**已完成** — `TcpServer` 使用 timerfd 周期唤醒 I/O 线程，`scanConnectionTimeouts()` 批量扫描并关闭超时连接，有界扫描防止阻塞。

### 6.3 心跳超时和断线清理

- [x] 实现心跳超时踢下线。
  - 优先级：P1
  - 状态：**已完成** — `heartbeat.timeout_ms` 配置，已认证连接超心跳超时后自动断线并清理 session。

- [x] 断线流程同步 Redis。
  - 优先级：P1
  - 状态：**已完成** — `RedisSessionStore::ClearPresence()` 校验 `connection_id` 后删除在线 key，避免误删新连接状态。

## 7. 系统安全性增强（进行中）

### 7.1 密码安全

- [ ] 替换 `std::hash` 密码哈希。
  - 优先级：P1
  - 预估工时：1.5-2 天
  - 依赖：UserService、UserRepository
  - 状态：**待实现** — 当前仍使用简单哈希，需迁移到 bcrypt 或 Argon2。
  - 技术要求：
    - 推荐使用 bcrypt 或 Argon2。
    - 每个用户独立 salt。
  - 实施步骤：
    - 新增 `PasswordHasher` 接口。
    - 注册时保存安全哈希。
    - 登录时验证哈希。
  - 验收标准：
    - 数据库不保存明文密码。
    - 相同密码不同用户哈希不同。
    - 旧测试更新为安全哈希验证。

### 7.2 Token 安全

- [x] 重构 token 生成机制。
  - 优先级：P1
  - 状态：**已完成** — token 由 `UserService::generateToken()` 生成，通过 `RedisSessionStore` 存储和校验，支持 TTL 过期，登出时 `RevokeToken()` 撤销。

- [ ] 增加重放保护策略。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：TokenService、Redis
  - 状态：**待实现** — 需在 token 中加入唯一 ID（jti），Redis 记录失效 token。
  - 技术要求：
    - 可基于 token jti 或 session version。
  - 实施步骤：
    - token 中包含唯一 ID。
    - Redis 记录有效 token 或失效 token。
  - 验收标准：
    - 登出后旧 token 不能继续使用。

### 7.3 SQL 安全

- [ ] 将用户 Repository 改为 prepared statement。
  - 优先级：P1
  - 预估工时：2 天
  - 依赖：DbConnection native handle
  - 状态：**待实现** — 当前仍使用 `mysql_real_escape_string` 拼接。
  - 技术要求：
    - 替换手动 `mysql_real_escape_string` 拼接。
    - 所有用户输入通过绑定参数传递。
  - 实施步骤：
    - 封装 MySQL prepared statement helper。
    - 改造 `findByUsername`、`findById`、`createUser`。
  - 验收标准：
    - 单引号、特殊字符用户名不会破坏 SQL。
    - 注入 payload 不会改变查询语义。

- [ ] 将消息 Repository 使用 prepared statement。
  - 优先级：P1
  - 预估工时：1.5 天
  - 依赖：MessageRepository
  - 状态：**待实现** — 当前消息内容仍使用转义拼接。
  - 技术要求：
    - 消息内容必须通过绑定参数写入。
  - 验收标准：
    - 任意文本消息不会造成 SQL 注入。

### 7.4 输入验证

- [ ] 建立统一输入验证模块。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：协议结构
  - 状态：**待实现** — 当前 Service 层有零散校验，需统一为 `Validator` 模块。
  - 技术要求：
    - username、password、nickname、message content 均有长度和字符规则。
  - 实施步骤：
    - 新增 `Validator` 工具。
    - 在 Service 层统一调用。
  - 验收标准：
    - 非法输入不会进入 Repository。
    - 所有错误返回稳定 `INVALID_PARAM` 或细分错误码。

## 8. 聊天业务模型完善（部分完成）

### 8.1 好友关系

- [ ] 设计好友表。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：MySQL schema
  - 状态：**待实现**
  - 技术要求：
    - 支持好友关系状态：pending、accepted、blocked。
  - 实施步骤：
    - 新增 `friendships` 表。
    - 增加唯一索引防止重复好友关系。
  - 验收标准：
    - 能表达申请、同意、删除关系。

- [ ] 实现好友接口。
  - 优先级：P2
  - 预估工时：2-3 天
  - 依赖：好友表
  - 状态：**待实现**
  - 技术要求：
    - 支持添加好友、同意好友、删除好友、好友列表。
  - 实施步骤：
    - 新增 `FriendRepository`、`FriendService`。
    - MessageHandler 增加协议分支。
  - 验收标准：
    - 非好友是否允许发消息的策略可配置或明确。

### 8.2 单聊会话

- [x] 完善单聊 conversation 管理。
  - 优先级：P1
  - 状态：**已完成** — `MessageRepository::findOrCreateSingleConversation()` 自动创建或复用同一对用户的单聊 conversation，`conversation_members` 关联表已定义。

### 8.3 群聊

- [ ] 设计群组和成员表。
  - 优先级：P2
  - 预估工时：1.5 天
  - 依赖：用户系统、消息表
  - 状态：**Schema 已就绪**（`conversation_members` 表支持群角色），需新增 `groups` 表和群管理逻辑。
  - 技术要求：
    - 表至少包含 `groups`、`group_members`。
  - 实施步骤：
    - 编写 schema。
    - 设计群角色：owner、admin、member。
  - 验收标准：
    - 能创建群并添加成员。

- [ ] 实现群消息群发。
  - 优先级：P2
  - 预估工时：3-5 天
  - 依赖：群组模型、主动推送、离线消息
  - 状态：**待实现**
  - 技术要求：
    - 在线成员推送。
    - 离线成员写离线消息。
    - 大群需要限制 fanout 策略，第一版可先支持小群。
  - 实施步骤：
    - 新增 `send_group_message`。
    - 查询群成员。
    - 对成员逐个投递或存储。
  - 验收标准：
    - 群内在线成员收到消息。
    - 离线成员上线后可拉取。

### 8.4 消息可靠性

- [ ] 设计消息 ACK 机制。
  - 优先级：P2
  - 预估工时：2 天
  - 依赖：消息状态管理
  - 状态：**待实现** — 服务端已有 `markDelivered` 能力，但缺少客户端 ACK 协议。
  - 技术要求：
    - 客户端收到推送后发送 ACK。
    - 服务端 ACK 后标记 delivered。
  - 实施步骤：
    - 新增 `message_ack` 协议。
    - 更新消息状态。
  - 验收标准：
    - 未 ACK 消息可重发或保留为未投递。

- [x] 实现消息去重。
  - 优先级：P2
  - 状态：**已完成** — `MessageDedupCache`（Redis）+ `client_msg_id` 唯一索引双重保障，`ChatService::sendMessage()` 先去重再入库。

- [ ] 实现消息序号。
  - 优先级：P2
  - 预估工时：2 天
  - 依赖：conversation 模型
  - 状态：**待实现**
  - 技术要求：
    - 每个 conversation 内消息序号单调递增。
  - 实施步骤：
    - 在 conversations 中维护 `last_seq`。
    - 插入消息时事务更新。
  - 验收标准：
    - 同一会话消息可以稳定排序。

### 8.5 多端登录

- [ ] 制定多端登录策略。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：Redis session
  - 状态：**待实现** — 当前为单端在线策略。
  - 技术要求：
    - 可选策略：单端在线、多端在线但每端独立 connection、多端同步消息。
  - 实施步骤：
    - 定义 `device_id`。
    - 在线状态从 `user_id -> connection` 扩展为 `user_id -> device_id -> connection`。
  - 验收标准：
    - 同一用户多设备在线状态清晰。

- [ ] 实现多端消息同步。
  - 优先级：P2
  - 预估工时：3-5 天
  - 依赖：多端登录策略、消息持久化
  - 状态：**待实现**
  - 技术要求：
    - 自己发送的消息同步到其他在线设备。
    - 每个设备独立 ACK。
  - 实施步骤：
    - 扩展消息状态为按设备维度。
    - 推送到同用户其他设备。
  - 验收标准：
    - 手机端发送消息后桌面端能收到同步。

## 推荐实施路线

### 第 1 周：安全加固（当前阶段）

- [ ] 引入 bcrypt/Argon2 替换简单密码哈希。
- [ ] 封装 MySQL prepared statement 并改造 UserRepository 和 MessageRepository。
- [ ] 建立统一输入验证模块。
- [ ] 实现已读回执协议接口。

验收标准：

- 密码存储达到安全哈希标准。
- SQL 注入测试不能绕过认证或破坏查询。
- 所有输入参数有统一校验，非法输入不进入 Repository。

### 第 2 周：消息可靠性增强

- [ ] 实现客户端消息 ACK 协议。
- [ ] 实现消息序号（conversation 内单调递增）。
- [ ] 实现重放保护策略。

验收标准：

- 客户端 ACK 后消息标记 delivered。
- 未 ACK 消息保留为未投递状态。
- 同一会话消息可按序号稳定排序。
- 登出后旧 token 不能继续使用。

### 第 3 周：好友关系与群聊基础

- [ ] 设计并实现好友表和好友接口。
- [ ] 设计群组表并实现群消息群发。

验收标准：

- 好友申请、同意、删除流程正常。
- 群内在线成员收到消息。
- 离线成员上线后可拉取群消息。

### 第 4 周：多端登录与产品打磨

- [ ] 制定并实现多端登录策略。
- [ ] 实现多端消息同步。
- [ ] 端到端集成测试覆盖核心路径。

验收标准：

- 同一用户多设备在线状态清晰。
- 多端消息同步正确。
- 主要业务路径有集成测试覆盖。

## 全局验收标准

- [x] `cmake --build build` 通过。
- [x] `ctest --test-dir build --output-on-failure` 通过。
- [x] 两个客户端可完成注册、登录、在线单聊、登出。
- [x] 目标用户离线时消息写入 MySQL，重新登录后可拉取。
- [x] Redis 中能看到在线状态和 session 信息。
- [x] 服务端日志有统一格式，关键错误可排查。
- [x] 空闲连接和心跳超时连接能自动清理。
- [ ] 密码、token、数据库密码不会以明文形式出现在日志中。（配置脱敏已完成，密码哈希待实现）
- [x] 主要业务路径有单元测试或集成测试覆盖。
