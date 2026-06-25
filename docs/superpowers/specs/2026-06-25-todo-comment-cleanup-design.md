# 待办标记与过期注释清理设计

## 目标

清理项目源码中的过期待办标记、失真注释和无效占位代码，使源码准确反映当前实现状态，并将确实未完成的工程事项集中维护在 `docs/roadmap.md`。

完成后，除 `third_party/` 外，仓库中的待办、修复和临时标记扫描结果应为零。

## 范围

本次审查覆盖：

- `include/`
- `src/`
- `tests/`
- `docs/`
- `README.md`
- CMake 构建文件

`third_party/nlohmann/json.hpp` 属于第三方代码，不修改其中的待办标记。

## 清理规则

每条现有待办标记 按以下规则处理：

1. 已有实现和测试覆盖的事项直接删除。
2. 与当前架构或行为不一致的注释改写为准确说明。
3. 没有明确需求或验收标准的愿望型事项直接删除，不进入 roadmap。
4. 确实未完成且对项目演进有价值的事项迁移到 `docs/roadmap.md`。
5. 源码不保留裸待办标记。未来新增待办必须进入 roadmap 或外部 issue，并提供范围和验收条件。

同时检查未显式标记，但使用“当前阶段”“后续”“预留”等措辞的注释，避免只删除标签而保留过期描述。

## 已完成事项

以下待办已被现有实现或测试覆盖，直接删除：

- `TcpServer` 已使用业务线程池处理阻塞业务。
- `TcpServer` 已实现 `stop()`、`eventfd` 唤醒和停机资源清理。
- `TcpServer` 已使用 `timerfd` 执行空闲和心跳超时扫描。
- `DbConfig` 已支持 JSON 和环境变量加载，并提供敏感配置脱敏输出。
- `DbPool` 已基于 MySQL C API 实现连接借还、RAII 归还、健康检查、重建和连接重试。
- `UserRepository` 已区分未命中、查询失败、连接不可用和借用超时。
- `PacketCodec` 已覆盖半包、CRLF、超大包和多包测试。
- `TcpConnection` 已支持移动构造、移动赋值和部分写入接口。
- `ConnectionMeta` 已包含认证用户、协议活跃时间和连接状态。
- `SessionManager` 已支持一个用户对应多个本地连接。
- `JsonCodec` 已统一将空响应数据编码为对象，并提供具体字段错误。
- `Message` 的来源连接信息已通过 `RequestContext` 传递。
- `UserId`、`ConnectionId` 与数据库及 `TcpServer` 内部编号的位宽已经一致。
- 认证、聊天和群组协议已经超出原待办描述的能力范围。

## 过期说明修正

下列文件中的模块说明需要根据现状改写：

- `include/db/db_pool.h`：删除“轻量封装、后续补充复用和线程安全”的过期描述。
- `include/config/db_config.h`：删除“后续连接池扩展”的过期描述。
- `include/codec/packet_codec.h`：将“计划使用换行协议”改为当前协议事实。
- `include/net/IMessageHandler.h`：修正 `handle()` 和 `onConnectionClosed()` 的线程与副作用说明。
- `include/net/TcpServer.h`：将工作线程池注释改为当前职责。
- 其他模型、协议和 Service 头文件：删除没有约束意义的愿望型注释。

## 占位与未使用代码

以下代码不代表当前产品能力，且没有有效调用方，直接删除：

- `include/EchoHandler.h`
- `src/EchoHandler.cc`
- `include/server/connection_manager.h`
- `include/protocol/protocol_helper.h`
- `src/protocol/protocol_helper.cc`

同步更新 `src/CMakeLists.txt` 和相关 include。响应构造仍由当前 `MessageHandler` 路径负责，本次不引入额外重构。

## Roadmap

新建 `docs/roadmap.md`，只记录以下已确认缺口：

### 信号驱动的优雅退出

为生产入口安装 `SIGINT` 和 `SIGTERM` 处理机制，安全唤醒服务器事件循环并复用现有停机路径。

验收条件：

- 收到 `SIGINT` 或 `SIGTERM` 后停止接受新连接。
- 停止外部 Redis 推送源。
- 等待已提交 worker 清理任务完成。
- 进程正常退出，并有自动化测试覆盖。

### 请求执行期间断连的副作用语义

明确 worker 正在执行时连接断开后，数据库写入、限流和 Redis 等业务副作用是否允许完成，以及响应阶段如何抑制无效会话副作用。

验收条件：

- 文档定义可提交和必须取消的副作用边界。
- 登录、发送消息等关键流程具有断连并发测试。
- 源码注释与实现语义一致。

### MessageHandler 按业务域拆分

在路由和依赖数量继续增长时，将认证、聊天、好友和群组处理拆分为独立组件。

验收条件：

- 顶层 handler 只负责信封解析、认证前置校验和路由。
- 子 handler 可独立测试。
- 对外协议和响应格式保持兼容。

### 请求可观测性

增加结构化请求日志和处理耗时，至少包括消息类型、连接 ID、结果码和耗时。

验收条件：

- 不记录密码、token 或消息正文。
- 成功、参数错误和内部错误路径均有稳定字段。
- 可通过测试或日志 sink 验证。

### 统一错误文案

建立错误码默认文案映射，减少 Service 和 Handler 中重复或不一致的字符串。

验收条件：

- 每个对外 `ErrorCode` 都有默认文案。
- 允许少量业务上下文覆盖，但不得泄露数据库细节。
- 协议测试验证错误码与文案稳定性。

### 连接状态迁移说明

记录连接从建立、活跃、半关闭、关闭中到已关闭的状态迁移和各关闭入口。

验收条件：

- 文档列出状态、事件和资源所有权。
- 关闭路径与 `ConnectionMeta::State` 一致。
- 覆盖重复关闭和停机期间关闭场景。

### 跨节点多端登录

将当前 Redis 用户级单 presence 扩展为设备或连接维度的多 presence。

验收条件：

- 明确 `device_id` 或等价稳定标识。
- 支持同一用户跨节点多个在线端。
- 登出和断连只清理对应在线端。
- 推送路由覆盖所有目标在线端。

### 时间字段模型统一

统一数据库时间、协议时间和运行时单调时间的类型与单位。

验收条件：

- 明确 wall clock 与 monotonic clock 的使用边界。
- 协议时间戳统一单位。
- Repository 不再用无类型字符串表示需要计算的时间。

## 验证

实施完成后执行：

```bash
rg -n --glob '!third_party/**' --glob '!build/**' 'T[D]O|FIXM[E]|X[X]X' .
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
mise run fix-md
mise run check-md
```

预期结果：

- 项目源码和文档中的待办标记扫描结果为空。
- 构建和全部测试通过。
- Markdown 检查通过。
- 删除的占位源码不再出现在构建目标或 include 引用中。
