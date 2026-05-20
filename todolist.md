# Linux C++ 聊天服务器后续开发 Todo List

本文档用于系统规划聊天服务器从“认证服务器 + MySQL 连接池”继续演进为“可演示的高并发聊天系统”的开发任务。

当前基础能力：

- 已具备 `epoll + 非阻塞 socket + worker 线程池` 网络框架。
- 已具备 JSON 协议、packet codec、基础请求响应模型。
- 已实现用户注册、登录、登出、心跳、`whoami`。
- 已实现进程内 session 管理。
- 已实现 MySQL 用户 Repository 和第一版生产级 MySQL 连接池。
- 已补充连接池结构化错误模型、日志统计和 fake-driven 单元测试。

后续核心目标：

- 实现在线单聊消息转发，形成最小聊天闭环。
- 支持离线消息持久化和拉取。
- 接入 Redis 管理在线状态、session 和缓存。
- 完善配置、日志、连接超时、安全认证和聊天业务模型。

## 阶段总览

| 阶段 | 模块 | 优先级 | 预估工时 | 依赖 |
| --- | --- | --- | --- | --- |
| 1 | 消息转发核心业务实现 | P0 | 3-5 天 | 现有网络层、SessionManager、MessageHandler |
| 2 | 离线消息与消息持久化系统 | P0 | 4-6 天 | 阶段 1、MySQL 连接池 |
| 3 | Redis 集成与分布式支持 | P1 | 5-8 天 | 阶段 1、配置系统基础 |
| 4 | 配置系统完善 | P1 | 2-3 天 | 现有启动入口、DB 配置 |
| 5 | 日志系统实现 | P1 | 3-5 天 | 配置系统基础 |
| 6 | 连接管理与超时回收 | P1 | 3-5 天 | 网络层连接元信息、日志系统 |
| 7 | 系统安全性增强 | P1 | 5-8 天 | 用户认证流程、MySQL Repository |
| 8 | 聊天业务模型完善 | P2 | 10-20 天 | 阶段 1-3 |

优先级说明：

- P0：形成聊天服务器最小可演示闭环必须完成。
- P1：让项目具备生产化亮点和稳定性必须完成。
- P2：扩展聊天产品能力，可在核心闭环后逐步实现。

## 1. 消息转发核心业务实现

### 1.1 协议与数据结构

- [ ] 定义 `send_message` 请求结构。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：现有 `common/message.h`、`protocol/auth_messages.h`
  - 技术要求：
    - 请求字段至少包含 `to_user_id`、`content`、`client_msg_id`。
    - `client_msg_id` 由客户端生成，用于幂等和排查。
    - `content` 需要限制最大长度，例如 4096 字节。
  - 实施步骤：
    - 新增 `ChatMessageRequest`、`ChatMessageResponseData`。
    - 在 JSON codec 或 MessageHandler 解析层读取字段。
    - 在 README 中补充协议示例。
  - 验收标准：
    - 缺少 `to_user_id` 时返回 `INVALID_PARAM`。
    - 缺少或超长 `content` 时返回 `INVALID_PARAM`。
    - 合法请求能进入 service 层。

- [ ] 定义服务端主动推送事件结构。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：packet codec、TcpServer 写队列
  - 技术要求：
    - 推送类型建议命名为 `message_push`。
    - 推送字段至少包含 `message_id`、`from_user_id`、`to_user_id`、`content`、`created_at`。
    - 推送包仍使用现有 JSON + packet codec。
  - 实施步骤：
    - 定义 `MessagePushData`。
    - 封装 `BuildMessagePush()` 辅助函数。
    - 添加 message handler 单元测试。
  - 验收标准：
    - 能生成合法 JSON 推送包。
    - 推送包可被现有 `PacketCodec` 编码并被客户端读取。

### 1.2 在线用户查找

- [ ] 扩展 SessionManager 支持 `user_id -> connection_id` 查询。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：现有 `SessionManager`
  - 技术要求：
    - 保持线程安全。
    - 支持按连接查 session，也支持按用户查连接。
  - 实施步骤：
    - 在 `ISessionManager` 增加 `GetConnectionByUserId(UserId)`。
    - 在 `SessionManager` 使用已有 `user_to_connection_` 实现查询。
    - 增加 session manager 单元测试。
  - 验收标准：
    - 用户登录绑定后可通过 `user_id` 查到连接。
    - 用户登出或连接断开后查询为空。

- [ ] 明确多端登录第一版策略。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：SessionManager
  - 技术要求：
    - 第一版建议采用“同一用户只允许一个在线连接”。
    - 新连接登录时踢掉旧连接，或旧连接被覆盖并清理。
  - 实施步骤：
    - 在文档中声明策略。
    - 保持 `BindSession` 当前覆盖旧连接行为。
    - 在网络层补充旧连接关闭策略任务。
  - 验收标准：
    - 同一用户重复登录时状态一致。
    - 不出现两个连接同时被视为同一用户在线。

### 1.3 主动推送链路

- [ ] 为 TcpServer 增加按 `connection_id` 主动推送接口。
  - 优先级：P0
  - 预估工时：1-1.5 天
  - 依赖：现有 `ConnectionContext`、响应队列、eventfd 唤醒机制
  - 技术要求：
    - 接口建议为 `bool pushToConnection(ConnectionId, std::string payload)`。
    - 推送必须由 I/O 线程安全地进入连接写队列。
    - worker 线程不得直接操作 socket。
  - 实施步骤：
    - 增加 `PushTask` 或复用 `ResponseTask`。
    - 通过 `completed_tasks_` 队列提交到 I/O 线程。
    - I/O 线程统一 `appendPendingSend()` 并启用 `EPOLLOUT`。
  - 验收标准：
    - worker 线程调用推送接口不会直接写 socket。
    - 目标连接存在时客户端能收到 `message_push`。
    - 目标连接不存在时返回失败，不崩溃。

- [ ] 将 MessageHandler 的返回模型扩展为支持“响应 + 额外推送”。
  - 优先级：P0
  - 预估工时：1 天
  - 依赖：主动推送接口
  - 技术要求：
    - `send_message` 需要给发送方返回 `send_message_resp`。
    - 同时向接收方推送 `message_push`。
  - 实施步骤：
    - 在 `HandleResult` 中增加 `push_tasks` 或类似结构。
    - TcpServer 处理 worker 结果时先写响应，再处理推送。
    - 增加 handler 测试。
  - 验收标准：
    - 发送方收到响应。
    - 在线接收方收到推送。
    - 推送失败时发送方能收到合理错误或降级为离线存储。

### 1.4 send_message 业务逻辑

- [ ] 实现 `send_message` 完整处理流程。
  - 优先级：P0
  - 预估工时：1-2 天
  - 依赖：在线用户查找、主动推送接口
  - 技术要求：
    - 发送方必须已登录。
    - 不能发送给无效用户。
    - 第一版可以允许给任意存在用户发送，好友限制放到后续阶段。
  - 实施步骤：
    - 在 `MessageHandler::handle()` 增加 `send_message` 分支。
    - 在 `UserService` 或新增 `ChatService` 中实现业务校验。
    - 在线目标用户走主动推送。
    - 离线目标用户进入离线消息存储。
  - 验收标准：
    - 未登录发送返回未认证错误。
    - 参数非法返回 `INVALID_PARAM`。
    - 目标在线时完成推送。
    - 目标离线时不丢消息，进入离线存储流程。

### 1.5 错误处理策略

- [ ] 建立消息转发错误码。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：`common/error_code.h`
  - 技术要求：
    - 至少区分未登录、目标用户不存在、目标离线、消息存储失败、推送失败。
  - 实施步骤：
    - 新增消息相关错误码段。
    - 补充 README 错误码说明。
  - 验收标准：
    - 每种失败场景有稳定错误码。
    - 客户端能根据错误码决定重试、提示或拉离线消息。

## 2. 离线消息与消息持久化系统

### 2.1 MySQL 表结构

- [ ] 设计 `conversations` 表。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：MySQL schema 管理方式
  - 技术要求：
    - 支持单聊和后续群聊扩展。
    - 建议字段：`id`、`type`、`created_at`、`updated_at`。
  - 实施步骤：
    - 新增 SQL schema 文件，例如 `sql/001_create_chat_tables.sql`。
    - 为 `type` 约定 `single`、`group` 枚举值。
  - 验收标准：
    - schema 可重复部署到测试库。
    - 单聊会话能唯一定位。

- [ ] 设计 `messages` 表。
  - 优先级：P0
  - 预估工时：0.5 天
  - 依赖：`conversations` 表
  - 技术要求：
    - 字段至少包含 `id`、`conversation_id`、`client_msg_id`、`from_user_id`、`to_user_id`、`content`、`status`、`created_at`、`delivered_at`、`read_at`。
    - 建立 `to_user_id + status + created_at` 索引用于拉取离线消息。
    - 对 `from_user_id + client_msg_id` 建唯一索引，支持幂等。
  - 实施步骤：
    - 编写 schema。
    - 增加消息状态枚举。
  - 验收标准：
    - 能插入单聊消息。
    - 重复 `client_msg_id` 不会重复入库。
    - 能按接收方查询未送达消息。

### 2.2 Repository 与 Service

- [ ] 新增 `MessageRepository`。
  - 优先级：P0
  - 预估工时：1-1.5 天
  - 依赖：MySQL 连接池
  - 技术要求：
    - 通过 `DbPool` 注入。
    - 使用结构化返回结果。
    - 第一版可以先使用转义 SQL，安全阶段再替换 prepared statement。
  - 实施步骤：
    - 定义 `IMessageRepository`。
    - 实现 `createMessage()`、`listOfflineMessages()`、`markDelivered()`、`markRead()`。
    - 增加 fake repository 便于 service 测试。
  - 验收标准：
    - 插入消息成功返回服务端 `message_id`。
    - 查询离线消息按时间顺序返回。
    - 数据库错误能映射为明确 Repository 状态。

- [ ] 新增 `ChatService`。
  - 优先级：P0
  - 预估工时：1 天
  - 依赖：MessageRepository、SessionManager
  - 技术要求：
    - 负责消息发送、离线存储、拉取离线消息。
    - Service 不直接操作 socket。
  - 实施步骤：
    - 设计 `SendMessageResult`、`PullOfflineMessagesResult`。
    - 接入在线状态查询。
    - 为 MessageHandler 提供业务结果。
  - 验收标准：
    - 在线消息和离线消息路径均可单元测试。
    - Service 层不依赖 TcpServer。

### 2.3 离线写入与拉取

- [ ] 实现目标用户离线时自动存储消息。
  - 优先级：P0
  - 预估工时：1 天
  - 依赖：ChatService、MessageRepository
  - 技术要求：
    - 离线消息状态初始为 `stored` 或 `sent`。
    - 返回发送方成功响应，表明消息已被服务器接收。
  - 实施步骤：
    - `send_message` 时目标不在线则写 MySQL。
    - 返回 `message_id` 和状态。
  - 验收标准：
    - 目标用户离线时消息不丢。
    - 目标用户重新登录后可拉取。

- [ ] 实现 `pull_offline_messages`。
  - 优先级：P0
  - 预估工时：1-1.5 天
  - 依赖：MessageRepository
  - 技术要求：
    - 请求字段建议包含 `limit`、`before_message_id` 或 `since_message_id`。
    - 返回消息列表，默认限制条数防止大响应。
  - 实施步骤：
    - 在 README 中补充协议。
    - 在 MessageHandler 增加分支。
    - 在 ChatService 调用 Repository 查询。
  - 验收标准：
    - 未登录拉取返回未认证。
    - 登录用户只拉取自己的离线消息。
    - 支持分页或限制条数。

### 2.4 消息状态管理

- [ ] 实现消息投递状态更新。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：主动推送链路、MessageRepository
  - 技术要求：
    - 状态至少包含 `stored`、`delivered`、`read`。
    - 在线推送成功后可标记 `delivered`。
  - 实施步骤：
    - 推送成功后更新 `delivered_at`。
    - 客户端拉离线消息后批量标记 delivered。
  - 验收标准：
    - 已推送消息不会重复作为未投递离线消息返回。
    - 状态更新时间正确。

- [ ] 实现已读回执接口。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：消息状态管理
  - 技术要求：
    - 请求字段包含 `message_id` 或批量 `message_ids`。
  - 实施步骤：
    - 新增 `mark_message_read` 协议。
    - 更新消息状态为 `read`。
  - 验收标准：
    - 发送方后续查询能看到消息已读状态。

## 3. Redis 集成与分布式支持

### 3.1 Redis 客户端与连接池

- [ ] 选择并接入 Redis C/C++ 客户端。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：CMake、系统依赖管理
  - 技术要求：
    - 建议使用 `hiredis` 作为第一版客户端。
    - CMake 中检测并链接 Redis client。
  - 实施步骤：
    - 增加 `include/redis`、`src/redis`。
    - 封装 `RedisConnection`。
    - 增加连接测试，无法连接时可跳过集成测试。
  - 验收标准：
    - 能执行 `PING`。
    - 错误能结构化返回。

- [ ] 实现 Redis 连接池。
  - 优先级：P1
  - 预估工时：1.5-2 天
  - 依赖：MySQL 连接池经验
  - 技术要求：
    - 支持最小连接数、最大连接数、借用超时、健康检查、停机。
    - 设计可复用但不要过度抽象。
  - 实施步骤：
    - 实现 `RedisPoolConfig`、`RedisPool`、`PooledRedisConnection`。
    - 编写 fake-driven 单元测试。
  - 验收标准：
    - 并发借还不超过容量上限。
    - Redis 断连时能丢弃坏连接。

### 3.2 在线状态与 session 迁移

- [ ] 设计 Redis 在线状态 key。
  - 优先级：P1
  - 预估工时：0.5 天
  - 依赖：Redis 连接
  - 技术要求：
    - 建议 key：`chat:online:user:{user_id}`。
    - value 包含 `server_id`、`connection_id`、`login_at`、`last_active_at`。
    - 设置 TTL，心跳刷新。
  - 实施步骤：
    - 定义序列化格式。
    - 增加 `OnlineStatusStore` 接口。
  - 验收标准：
    - 登录写入在线状态。
    - 登出和断线删除在线状态。
    - 心跳刷新 TTL。

- [ ] 将 session 存储抽象为接口。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：现有 `ISessionManager`
  - 技术要求：
    - 保留本地内存实现用于单进程测试。
    - 新增 Redis 实现用于分布式部署。
  - 实施步骤：
    - 拆分 `SessionManager` 与 `SessionStore`。
    - `UserService` 面向接口。
  - 验收标准：
    - 原有测试不受影响。
    - Redis 实现通过 fake Redis 或集成测试。

### 3.3 分布式消息路由

- [ ] 设计多节点在线路由模型。
  - 优先级：P1
  - 预估工时：1-2 天
  - 依赖：Redis 在线状态
  - 技术要求：
    - 当前节点只能直接推送本节点连接。
    - 跨节点推送需要消息通道。
  - 实施步骤：
    - 每个服务启动生成 `server_id`。
    - 在线状态记录目标用户所在 `server_id`。
    - 同节点直接推送，跨节点通过 Redis Pub/Sub 或 Stream。
  - 验收标准：
    - 两个 server 进程能共享在线状态。
    - 跨节点消息能送达目标连接。

- [ ] 接入 Redis Pub/Sub 或 Stream 作为跨节点投递通道。
  - 优先级：P1
  - 预估工时：2-3 天
  - 依赖：多节点在线路由
  - 技术要求：
    - 第一版可用 Pub/Sub。
    - 若需要可靠性，后续升级 Redis Stream。
  - 实施步骤：
    - 节点订阅 `chat:server:{server_id}:push`。
    - 跨节点发送时 publish 到目标 server channel。
    - 接收后进入本地 `pushToConnection()`。
  - 验收标准：
    - 跨进程发送单聊消息成功。
    - 目标节点不可达时降级为离线存储。

### 3.4 缓存策略

- [ ] 缓存用户基础信息。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：Redis pool、UserRepository
  - 技术要求：
    - 缓存 username 到 user_id、user_id 到 nickname/status。
    - 设置 TTL，更新用户信息时删除缓存。
  - 实施步骤：
    - 新增 `UserCache`。
    - UserService 查询用户时先查缓存再查 MySQL。
  - 验收标准：
    - 缓存命中时不访问 MySQL。
    - 缓存失效后可回源。

## 4. 配置系统完善

### 4.1 配置文件格式

- [ ] 选择并定义配置文件格式。
  - 优先级：P1
  - 预估工时：0.5 天
  - 依赖：第三方库选择
  - 技术要求：
    - 建议第一版使用 JSON，复用 `nlohmann/json`。
    - 文件建议为 `config/server.json`。
  - 实施步骤：
    - 定义 server、mysql、redis、log、timeout 配置段。
    - 提供 `config/server.example.json`。
  - 验收标准：
    - 示例配置可直接启动本地开发服务。

### 4.2 配置加载与校验

- [ ] 实现 `ConfigLoader`。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：配置格式
  - 技术要求：
    - 支持配置文件路径参数。
    - 支持默认值。
    - 支持环境变量覆盖敏感字段。
  - 实施步骤：
    - 新增 `include/config/server_config.h`。
    - 新增 `src/config/config_loader.cc`。
    - 将 `LoadDbConfigFromEnv()` 迁移到配置模块。
  - 验收标准：
    - 配置缺失时有明确错误。
    - 端口、连接池大小、超时等字段被校验。

- [ ] 将硬编码配置迁移到配置文件。
  - 优先级：P1
  - 预估工时：0.5-1 天
  - 依赖：ConfigLoader
  - 技术要求：
    - 服务器监听 IP、端口、MySQL、Redis、日志路径均来自配置。
  - 实施步骤：
    - 修改 `main.cc`。
    - 保留环境变量兼容或作为覆盖机制。
  - 验收标准：
    - 修改配置文件即可改变监听端口和数据库地址。

### 4.3 敏感信息脱敏

- [ ] 实现配置脱敏输出。
  - 优先级：P1
  - 预估工时：0.5 天
  - 依赖：日志系统或临时输出函数
  - 技术要求：
    - password、token secret、Redis password 等字段不得明文输出。
  - 实施步骤：
    - 为配置结构提供 `toSafeString()`。
    - 启动日志只打印脱敏配置。
  - 验收标准：
    - 日志中搜索不到明文密码。

## 5. 日志系统实现

### 5.1 Logger 基础能力

- [ ] 实现轻量级 Logger。
  - 优先级：P1
  - 预估工时：1-1.5 天
  - 依赖：配置系统基础
  - 技术要求：
    - 支持 `DEBUG`、`INFO`、`WARN`、`ERROR`。
    - 每条日志包含时间戳、线程 ID、级别、模块名、消息。
  - 实施步骤：
    - 新增 `include/common/logger.h`。
    - 新增 `src/common/logger.cc`。
    - 提供 `LOG_INFO("module") << ...` 或简单函数式接口。
  - 验收标准：
    - 能控制最低输出级别。
    - 日志格式稳定。

- [ ] 替换现有标准输出。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：Logger
  - 技术要求：
    - 替换 `std::cout`、`std::cerr`、`perror`。
    - MySQL 密码等敏感信息不得进入日志。
  - 实施步骤：
    - 先替换 DB pool、TcpServer、Repository。
    - 再替换 tests 中不必要的输出。
  - 验收标准：
    - `rg "std::cout|std::cerr|perror" src include` 只剩合理例外。

### 5.2 文件输出与异步日志

- [ ] 支持日志文件输出和滚动。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：Logger、配置系统
  - 技术要求：
    - 支持按大小滚动。
    - 支持保留最近 N 个日志文件。
  - 实施步骤：
    - 增加 file sink。
    - 配置 `log.file_path`、`max_size_mb`、`max_files`。
  - 验收标准：
    - 日志文件达到阈值后滚动。

- [ ] 支持异步日志写入。
  - 优先级：P2
  - 预估工时：1.5-2 天
  - 依赖：线程安全队列
  - 技术要求：
    - 业务线程只入队，不阻塞写磁盘。
    - 停机时 flush。
  - 实施步骤：
    - 新增日志后台线程。
    - 停机时 drain 队列。
  - 验收标准：
    - 高并发请求时日志不明显阻塞业务。
    - 进程正常退出时日志不丢。

## 6. 连接管理与超时回收

### 6.1 连接元信息完善

- [ ] 补齐连接元信息。
  - 优先级：P1
  - 预估工时：0.5 天
  - 依赖：现有 `ConnectionMeta`
  - 技术要求：
    - 包含连接创建时间、最后收包时间、最后发包时间、认证用户 ID。
  - 实施步骤：
    - 扩展 `ConnectionMeta`。
    - 在收发和 session 绑定时更新。
  - 验收标准：
    - 日志能输出连接生命周期关键信息。

### 6.2 空闲连接回收

- [ ] 实现定时扫描机制。
  - 优先级：P1
  - 预估工时：1.5 天
  - 依赖：TcpServer 停机机制
  - 技术要求：
    - 定时扫描不能阻塞 I/O 线程太久。
    - 可使用后台 timer 线程唤醒 I/O 线程执行关闭。
  - 实施步骤：
    - 配置 `connection.idle_timeout_ms`。
    - 找出超时连接并调用安全关闭路径。
  - 验收标准：
    - 空闲连接超过阈值后被关闭。
    - 正在收发数据的连接不会误关闭。

### 6.3 心跳超时和断线清理

- [ ] 实现心跳超时踢下线。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：心跳协议、连接元信息
  - 技术要求：
    - 心跳刷新 `last_active_at`。
    - 超时后清理 session 和在线状态。
  - 实施步骤：
    - 配置 `heartbeat.timeout_ms`。
    - 扫描超时连接。
  - 验收标准：
    - 客户端停止心跳后自动断线。
    - `whoami` 不再能查到旧 session。

- [ ] 断线流程同步 Redis。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：Redis 在线状态
  - 技术要求：
    - 连接关闭必须删除 Redis 在线 key。
    - 避免误删新连接的在线状态。
  - 实施步骤：
    - 在线状态 value 中加入 `connection_id`。
    - 删除时校验当前 key 的 connection_id。
  - 验收标准：
    - 断线后在线状态及时消失。
    - 同用户新连接不会被旧连接关闭流程误删。

## 7. 系统安全性增强

### 7.1 密码安全

- [ ] 替换 `std::hash` 密码哈希。
  - 优先级：P1
  - 预估工时：1.5-2 天
  - 依赖：UserService、UserRepository
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

- [ ] 重构 token 生成机制。
  - 优先级：P1
  - 预估工时：1-2 天
  - 依赖：配置系统、SessionManager
  - 技术要求：
    - token 需要高随机性。
    - 支持过期时间。
    - 支持签名或服务端存储校验。
  - 实施步骤：
    - 新增 `TokenService`。
    - 登录时生成 token 并写 session。
    - 请求时校验 token 和 session。
  - 验收标准：
    - 伪造 token 不能通过认证。
    - 过期 token 被拒绝。

- [ ] 增加重放保护策略。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：TokenService、Redis
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
  - 技术要求：
    - 消息内容必须通过绑定参数写入。
  - 验收标准：
    - 任意文本消息不会造成 SQL 注入。

### 7.4 输入验证

- [ ] 建立统一输入验证模块。
  - 优先级：P1
  - 预估工时：1 天
  - 依赖：协议结构
  - 技术要求：
    - username、password、nickname、message content 均有长度和字符规则。
  - 实施步骤：
    - 新增 `Validator` 工具。
    - 在 Service 层统一调用。
  - 验收标准：
    - 非法输入不会进入 Repository。
    - 所有错误返回稳定 `INVALID_PARAM` 或细分错误码。

## 8. 聊天业务模型完善

### 8.1 好友关系

- [ ] 设计好友表。
  - 优先级：P2
  - 预估工时：1 天
  - 依赖：MySQL schema
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
  - 技术要求：
    - 支持添加好友、同意好友、删除好友、好友列表。
  - 实施步骤：
    - 新增 `FriendRepository`、`FriendService`。
    - MessageHandler 增加协议分支。
  - 验收标准：
    - 非好友是否允许发消息的策略可配置或明确。

### 8.2 单聊会话

- [ ] 完善单聊 conversation 管理。
  - 优先级：P1
  - 预估工时：2 天
  - 依赖：conversations 表、messages 表
  - 技术要求：
    - 同一对用户复用同一个单聊 conversation。
  - 实施步骤：
    - 增加 conversation participant 表或唯一 key。
    - 发送消息时自动创建或查找 conversation。
  - 验收标准：
    - 两个用户之间消息归属同一会话。
    - 能按 conversation 拉历史消息。

### 8.3 群聊

- [ ] 设计群组和成员表。
  - 优先级：P2
  - 预估工时：1.5 天
  - 依赖：用户系统、消息表
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
  - 技术要求：
    - 客户端收到推送后发送 ACK。
    - 服务端 ACK 后标记 delivered。
  - 实施步骤：
    - 新增 `message_ack` 协议。
    - 更新消息状态。
  - 验收标准：
    - 未 ACK 消息可重发或保留为未投递。

- [ ] 实现消息去重。
  - 优先级：P2
  - 预估工时：1.5 天
  - 依赖：`client_msg_id` 唯一索引
  - 技术要求：
    - 同一发送方重复提交同一 `client_msg_id` 返回同一结果。
  - 实施步骤：
    - Repository 捕获唯一键冲突。
    - 查询已有消息返回。
  - 验收标准：
    - 客户端重试不会产生重复消息。

- [ ] 实现消息序号。
  - 优先级：P2
  - 预估工时：2 天
  - 依赖：conversation 模型
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
  - 技术要求：
    - 自己发送的消息同步到其他在线设备。
    - 每个设备独立 ACK。
  - 实施步骤：
    - 扩展消息状态为按设备维度。
    - 推送到同用户其他设备。
  - 验收标准：
    - 手机端发送消息后桌面端能收到同步。

## 推荐实施路线

### 第 1 周：单聊在线闭环

- [ ] 定义 `send_message` 和 `message_push` 协议。
- [ ] 实现 `user_id -> connection_id` 查询。
- [ ] 实现 TcpServer 主动推送接口。
- [ ] 实现在线单聊消息转发。
- [ ] 增加在线转发集成测试。

验收标准：

- 两个客户端同时在线时，A 发送消息，B 能收到服务端主动推送。
- A 能收到发送响应。
- 未登录用户不能发送消息。

### 第 2 周：离线消息闭环

- [ ] 创建 `conversations` 和 `messages` 表。
- [ ] 实现 `MessageRepository`。
- [ ] 实现离线消息写入。
- [ ] 实现 `pull_offline_messages`。
- [ ] 增加离线消息测试。

验收标准：

- B 离线时 A 发送消息，消息写入 MySQL。
- B 登录后调用 `pull_offline_messages` 能拉到消息。
- 拉取后消息状态更新为 delivered。

### 第 3 周：Redis 与超时管理

- [ ] 接入 Redis 客户端。
- [ ] 实现 Redis 连接池。
- [ ] 将在线状态写入 Redis。
- [ ] 心跳刷新在线状态 TTL。
- [ ] 实现空闲连接扫描和心跳超时踢下线。

验收标准：

- 登录后 Redis 可查在线状态。
- 断线或超时后 Redis 在线状态被清理。
- 两个服务进程能共享在线状态。

### 第 4 周：工程化增强

- [ ] 引入统一配置文件。
- [ ] 实现 Logger 并替换标准输出。
- [ ] 实现密码安全哈希。
- [ ] 重构 token 生成和校验。
- [ ] 将用户 Repository 改为 prepared statement。

验收标准：

- 服务启动完全由配置驱动。
- 日志格式统一且不泄漏敏感信息。
- 密码和 token 达到真实认证基本安全要求。
- SQL 注入测试不能绕过认证或破坏查询。

## 全局验收标准

- [ ] `cmake --build build` 通过。
- [ ] `ctest --test-dir build --output-on-failure` 通过。
- [ ] 两个客户端可完成注册、登录、在线单聊、登出。
- [ ] 目标用户离线时消息写入 MySQL，重新登录后可拉取。
- [ ] Redis 中能看到在线状态和 session 信息。
- [ ] 服务端日志有统一格式，关键错误可排查。
- [ ] 空闲连接和心跳超时连接能自动清理。
- [ ] 密码、token、数据库密码不会以明文形式出现在日志中。
- [ ] 主要业务路径有单元测试或集成测试覆盖。

