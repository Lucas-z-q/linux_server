# 线程池引入方案 Todo List

## 目标

当前项目的 `TcpServer` 在 `epoll` 事件循环线程内直接执行：

`onReadable -> MessageHandler::handle -> UserService -> UserRepository`

其中 `UserRepository` 会进行阻塞式数据库访问。随着连接数和请求数增加，这会导致网络线程被业务处理和数据库调用阻塞，影响整个服务端的吞吐和响应延迟。

本次改造目标：

- 保持当前 `epoll + 非阻塞 socket` 的 I/O 模型不变
- 引入业务线程池，将消息处理从 I/O 线程剥离
- 保持 socket 收发仍由 I/O 线程统一负责，避免多线程直接操作 fd
- 为后续数据库连接池、异步业务扩展打基础

## 总体思路

建议采用“单 I/O 线程 + 业务线程池 + 完成队列”的模型。

在连接生命周期管理上，优先采用：

- `std::shared_ptr<ConnectionContext>` 持有活跃连接状态
- 在线程任务和完成结果中只传递 `std::weak_ptr<ConnectionContext>`

而不是优先依赖 `conn_id + generation`。

`conn_id` 仍然可以保留用于日志、监控和排障，但“连接是否还活着”应优先由 `weak_ptr.lock()` 判定。

职责划分如下：

- I/O 线程：
  - `accept/read/write/close`
  - 拆包
  - 将完整请求投递到线程池
  - 从完成队列取出响应并追加到待发送缓冲
- 工作线程：
  - 执行业务处理
  - 调用 `MessageHandler::handle`
  - 生成响应字符串
  - 将结果投递回完成队列

关键原则：

- 工作线程不直接调用 `send`
- 工作线程不直接关闭连接
- 连接生命周期和发送缓冲区仍由 `TcpServer` 主线程统一管理
- 任何异步结果都必须在回到 I/O 线程后再决定是否发送
- `weak_ptr.lock()` 只用于判断连接是否仍然存在，不意味着 worker 可以直接操作连接 fd

## 推荐新增组件

### 1. 线程池

建议新增：

- `include/concurrency/thread_pool.h`
- `src/concurrency/thread_pool.cc`

线程池能力建议保持最小可用：

- 构造时指定线程数
- `start()`
- `stop()`
- `submit(Task)`
- 内部任务队列
- `condition_variable + mutex`
- 支持优雅停机

任务函数可以先用 `std::function<void()>`，避免一开始引入过多模板复杂度。

### 2. 请求任务结构

建议新增一个轻量结构，例如：

- `RequestTask`
  - `std::weak_ptr<ConnectionContext> connection`
  - `ConnectionId conn_id`
  - `std::string request`

说明：

- `conn_id` 主要用于日志、排障和业务接口兼容
- 真正的生命周期判断依赖 `connection.lock()`
- 第一版通常不需要额外引入 `generation`

### 3. 完成结果结构

建议新增一个轻量结构，例如：

- `ResponseTask`
  - `std::weak_ptr<ConnectionContext> connection`
  - `ConnectionId conn_id`
  - `std::string response`

后续如果需要支持“仅通知关闭连接”或“异步广播”，可以在这里继续扩展字段。

### 3.1 连接上下文对象

建议不要继续把连接状态分散在多个 `unordered_map` 中单独维护，而是新增一个由智能指针托管的连接上下文对象，例如：

- `ConnectionContext`
  - `ConnectionId conn_id`
  - `int fd`
  - `std::string pending_send`
  - `chat::PacketCodec packet_codec`
  - `ConnectionMeta meta`

推荐职责：

- `TcpServer` 持有活跃连接的 `shared_ptr`
- 异步任务只持有 `weak_ptr`
- 当连接关闭且主线程从活跃连接表移除后，若没有其他 `shared_ptr` 持有者，该连接状态会自然析构

这样可以天然避免：

- 旧任务命中“新 fd”
- 旧任务命中“新连接复用的逻辑标识”
- 单纯依赖 `conn_id` 或代次判断带来的额外管理复杂度

### 4. 完成队列唤醒机制

建议使用 `eventfd` 通知 `epoll` 线程“有新的业务处理结果”。

原因：

- 与当前 `epoll` 模型契合
- 比轮询简单高效
- 能让 I/O 线程继续保持事件驱动

可新增：

- `eventfd` 文件描述符
- 一个线程安全的完成队列
- `TcpServer` 中对该 fd 的 `EPOLLIN` 监听

关键细节：

- worker 每次投递完成结果后，向 `eventfd` 写入一个 `uint64_t(1)`
- I/O 线程收到 `EPOLLIN` 后必须读取 `eventfd`
- 读取后应立即批量消费完成队列，而不是一次只取一个结果
- 推荐做法是把共享完成队列 `swap` 到局部队列中，再在锁外处理

## 分步实现清单

### 第一阶段：抽出通用线程池

- [ ] 新增线程池类，支持提交 `std::function<void()>`
- [ ] 支持固定数量 worker 线程
- [ ] 支持析构或 `stop()` 时停止接收新任务
- [ ] 支持清晰的停机语义
  - 推荐：不再接收新任务，但允许已入队任务跑完
- [ ] 编写线程池单测

这一阶段先不接入 `TcpServer`，单独把基础设施写稳。

### 第二阶段：在 `TcpServer` 中接入业务任务投递

- [ ] 新增 `ConnectionContext` 结构，收口连接级状态
- [ ] 在 `TcpServer` 中增加线程池成员
- [ ] 在 `TcpServer` 中将活跃连接表改为持有 `shared_ptr<ConnectionContext>`
- [ ] 在 `TcpServer` 中增加请求投递逻辑
- [ ] 将当前 `onReadable` 中的同步调用
  - `handler_.handle(packet, conn_id)`
  - 改为投递到线程池
- [ ] 保留当前拆包逻辑不变
- [ ] 保留当前连接关闭逻辑不变

这一阶段的目标是“请求异步处理”，但先不急着让 worker 直接触发网络发送。

### 第三阶段：增加完成队列与主线程回投

- [ ] 在 `TcpServer` 中新增线程安全完成队列
- [ ] 新增 `eventfd` 并注册到 `epoll`
- [ ] worker 线程在业务处理完成后：
  - 将 `ResponseTask` 放入完成队列
  - 向 `eventfd` 写入唤醒信号
- [ ] I/O 线程新增 `onWorkerResultReadable()` 之类的方法
- [ ] I/O 线程收到唤醒后先读取 `eventfd` 计数器
- [ ] I/O 线程收到唤醒后批量消费完成队列
- [ ] 完成队列通过 `swap` 到局部变量后，在锁外处理
- [ ] 对每条结果执行：
  - 对 `weak_ptr` 执行 `lock()`
  - 若拿到连接对象，则编码并加入该连接的 `pending_send`
  - 打开 `EPOLLOUT`
  - 若 `lock()` 失败，则直接丢弃响应

这是整个方案的核心闭环。

### 第四阶段：连接失效与并发安全补强

- [ ] 确认连接状态是否已经完整收口到 `ConnectionContext`
- [ ] 确认异步任务只持有 `weak_ptr`，不意外延长连接生命周期
- [ ] 明确连接关闭时如何处理未完成任务
  - 推荐：允许任务完成，但发送前通过 `weak_ptr.lock()` 检查连接是否仍存在
- [ ] 确认 `SessionManager` 的线程安全边界
- [ ] 确认 `MessageHandler`、`UserService` 的共享使用是安全的
- [ ] 确认完成队列、待发送缓冲、fd 映射的锁粒度不会造成死锁
- [ ] 明确 worker 即使 `lock()` 成功，也不直接操作 fd 或 `epoll`

### 第五阶段：停机与资源回收

- [ ] `TcpServer::stop()` 中增加线程池停止流程
- [ ] 确保先停止接收新连接/新任务，再回收线程池
- [ ] 明确 `eventfd` 的关闭顺序
- [ ] 确保不会在 `epoll` 已关闭后继续往完成队列回投结果
- [ ] 为重复调用 `stop()` 保持幂等

## 实际开发顺序

下面这份顺序更适合真正开工时照着推进，目标是先把最稳定、最独立的部分做完，再逐步把并发能力接进主链路。

### Step 1：先抽线程池基础设施

- [ ] 新增 `ThreadPool` 头文件和实现文件
- [ ] 只支持最小功能：
  - 固定 worker 数量
  - `submit`
  - `stop`
  - 等待任务
- [ ] 先不接入 `TcpServer`
- [ ] 先写 `thread_pool_test`

完成标准：

- 能独立编译
- 单测通过
- 停机语义明确

### Step 2：抽连接上下文对象

- [ ] 新增 `ConnectionContext`
- [ ] 把原来分散在 `TcpServer` 内部的连接级状态识别出来
  - `fd`
  - `conn_id`
  - `pending_send`
  - `packet_codec`
  - `ConnectionMeta`
- [ ] 决定哪些状态保留在 `TcpServer`，哪些迁移到 `ConnectionContext`

建议目标：

- 连接自身的数据尽量放进 `ConnectionContext`
- `TcpServer` 只保留“活跃连接表”和事件循环控制逻辑

### Step 3：把活跃连接表改成 `shared_ptr`

- [ ] `TcpServer` 改为持有 `shared_ptr<ConnectionContext>`
- [ ] 新连接建立时创建 `shared_ptr`
- [ ] 连接关闭时从活跃连接表移除
- [ ] 保持当前同步业务逻辑不变，先确保连接管理改造本身是稳定的

这一步先不要引入线程池投递，重点是把生命周期托管模型先立起来。

完成后建议先做一次回归：

- [ ] 编译通过
- [ ] 原有测试不过多回退
- [ ] 单连接收发行为不变

### Step 4：接入线程池请求投递

- [ ] 定义 `RequestTask`
  - `weak_ptr<ConnectionContext>`
  - `conn_id`
  - `request`
- [ ] 在 `onReadable` 拆出完整包后，不再同步执行 `handler_.handle`
- [ ] 改为把任务投递到线程池
- [ ] worker 内部执行：
  - `connection.lock()`
  - 若连接已失效，直接放弃处理或尽早返回
  - 若连接仍有效，执行 `handler_.handle`

这里的重点不是发送，而是先把“异步处理请求”跑通。

### Step 5：接入完成队列和 `eventfd`

- [ ] 定义 `ResponseTask`
  - `weak_ptr<ConnectionContext>`
  - `conn_id`
  - `response`
- [ ] 在 `TcpServer` 中增加完成队列
- [ ] 增加 `eventfd`
- [ ] 将 `eventfd` 注册到 `epoll`
- [ ] worker 完成业务处理后：
  - `std::move(response)` 放入完成队列
  - 写 `eventfd`

这是把线程池真正接回网络层的关键一步。

### Step 6：在 I/O 线程消费完成结果

- [ ] 新增 `onWorkerResultReadable()`
- [ ] 在该函数中先读取 `eventfd`
- [ ] 把完成队列整体 `swap` 到局部变量
- [ ] 在锁外逐个处理结果
- [ ] 对每个结果：
  - `weak_ptr.lock()`
  - 成功则追加到连接发送缓冲
  - 开启 `EPOLLOUT`
  - 失败则丢弃

完成这一步后，整条异步闭环才算真正成立。

### Step 7：收尾并发边界

- [ ] 检查 worker 是否有任何直接操作 fd 的路径
- [ ] 检查连接关闭时是否可能与回投结果竞争
- [ ] 检查锁顺序，避免潜在死锁
- [ ] 明确消息是否允许乱序返回
- [ ] 检查 `stop()` 时线程池、`eventfd`、`epoll` 的关闭顺序

### Step 8：做回归和并发测试

- [ ] 跑现有单测和集成测试
- [ ] 补 `thread_pool_test`
- [ ] 补“连接关闭后结果被丢弃”的测试
- [ ] 补“多个完成结果一次唤醒批量处理”的测试
- [ ] 补并发连接/并发请求测试
- [ ] 做一次手工联调

## 开工时的优先级建议

如果按“最不容易把系统搞坏”的方式推进，推荐优先级如下：

1. `ThreadPool`
2. `ConnectionContext`
3. 活跃连接表改造为 `shared_ptr`
4. 请求异步投递
5. 完成队列
6. `eventfd`
7. I/O 线程回投发送
8. 停机与异常处理
9. 并发与回归测试

## 不建议一口气同时做的事情

为了降低回归风险，下面这些改动不要和线程池首版一起混做：

- [ ] 数据库连接池
- [ ] 消息严格顺序保证
- [ ] 多 I/O 线程模型
- [ ] 业务线程池分层
- [ ] 大规模协议重构

建议先把“单 I/O 线程 + 业务线程池 + `weak_ptr` 生命周期管理”这一版做稳，再继续演进。

## 建议修改位置

### 必改文件

- [ ] [src/TcpServer.cc](/home/lzq/coding/linux_server/src/TcpServer.cc)
- [ ] [include/net/TcpServer.h](/home/lzq/coding/linux_server/include/net/TcpServer.h)
- [ ] [CMakeLists.txt](/home/lzq/coding/linux_server/CMakeLists.txt)

### 建议新增文件

- [ ] [include/concurrency/thread_pool.h](/home/lzq/coding/linux_server/include/concurrency/thread_pool.h)
- [ ] [src/concurrency/thread_pool.cc](/home/lzq/coding/linux_server/src/concurrency/thread_pool.cc)
- [ ] [include/net/connection_context.h](/home/lzq/coding/linux_server/include/net/connection_context.h)

### 可能需要补充的测试文件

- [ ] [tests/thread_pool_test.cc](/home/lzq/coding/linux_server/tests/thread_pool_test.cc)
- [ ] [tests/server_integration_test.cc](/home/lzq/coding/linux_server/tests/server_integration_test.cc)
- [ ] [tests/auth_integration_test.cc](/home/lzq/coding/linux_server/tests/auth_integration_test.cc)

## 设计注意点

### 1. 优先用 `shared_ptr/weak_ptr` 管理连接生命周期

推荐模型：

- `TcpServer` 的活跃连接表持有 `shared_ptr<ConnectionContext>`
- `RequestTask` / `ResponseTask` 中只保存 `weak_ptr<ConnectionContext>`
- I/O 线程回收结果时通过 `weak_ptr.lock()` 判断连接是否仍有效

这样比单纯依赖 `conn_id` 或 `generation` 更自然，也更不容易出错。

`conn_id` 仍然有价值，但应主要用于：

- 日志
- 会话标识
- 监控统计
- 业务接口兼容

如未来确实有特殊复用场景，再考虑补充 `generation`，但不是第一优先项。

### 2. 不要让 worker 线程直接发包

如果工作线程直接操作 socket，会立刻把当前单线程 I/O 模型变成“多线程并发碰 fd”，复杂度会上升很多，包括：

- 写事件竞争
- 连接关闭竞争
- 缓冲区一致性问题
- `epoll` 状态修改竞争

因此必须坚持：

- worker 只产出结果
- I/O 线程统一发送

### 3. 不要在第一版里同时引入数据库连接池

线程池和数据库连接池都是并发改造点，最好拆开做。

第一版先接受“多个 worker 各自调用当前 `UserRepository` 建连逻辑”：

- 实现简单
- 更容易定位问题
- 改动边界清晰

当线程池稳定后，再继续演进数据库连接池。

### 4. 充分使用 Move 语义，先不要过早引入复杂所有权模型

`RequestTask` 和 `ResponseTask` 中的 `std::string` 会在多个阶段流转，因此要尽量减少不必要拷贝。

建议第一版采用：

- 按值接收任务对象
- 入队时 `std::move`
- 出队时 `std::move`
- worker 产出结果时继续 `std::move`

例如关注以下流转点：

- `packet` 放入 `RequestTask`
- `RequestTask` 放入任务队列
- worker 从任务队列取出任务
- `response` 放入 `ResponseTask`
- `ResponseTask` 放入完成队列

第一版不建议为了“零拷贝”过早引入：

- `std::unique_ptr<std::string>`
- `std::shared_ptr<Message>`

原因：

- 可读性会下降
- 额外堆分配和引用计数未必更划算
- 当前链路本身也还不是严格零拷贝

### 5. 注意消息顺序语义

同一个连接如果连续发送多条请求，异步处理后响应返回顺序可能和请求到达顺序不同。

这在当前协议里未必是错，但必须明确：

- 如果系统允许乱序响应，现状即可
- 如果希望同连接内严格按请求顺序返回，需要额外设计串行队列或序号屏障

建议第一版先接受“可能乱序”，但在文档和测试中明确这一点。

### 6. `eventfd` 的读写必须完整闭环

建议明确以下规则：

- worker 每完成一次结果投递，向 `eventfd` 写入一个 8 字节整数
- I/O 线程收到 `EPOLLIN` 后必须读取 `eventfd`
- 如果采用非阻塞模式，可循环读取直到 `EAGAIN`
- 读取完成后，批量处理完成队列

如果只写不读，特别是在 LT 模式下，`epoll_wait` 会反复被唤醒。

### 7. 关注锁的边界

建议遵循以下规则：

- 取任务时只持有任务队列锁
- 操作完成队列时只持有完成队列锁
- 操作单连接 `pending_send` 时只持有对应发送缓冲锁或连接状态锁
- 不在持锁时执行数据库访问或业务处理
- 完成队列处理采用“锁内 swap，锁外遍历”

目标是避免“锁内做慢操作”。

## 测试方案

测试应分为四层：线程池单测、`TcpServer` 行为测试、认证链路回归测试、压力与异常测试。

### 一、线程池单元测试

新增 `thread_pool_test`，至少覆盖以下场景：

- [ ] 能正常启动并执行单个任务
- [ ] 能执行多个任务
- [ ] 多个 worker 确实可以并发执行任务
- [ ] `stop()` 后不再接受新任务
- [ ] 已入队任务在停机前可以执行完成
- [ ] 析构时不会卡死
- [ ] 任务抛异常时不会导致整个线程池线程提前退出
  - 如果第一版不处理异常，也要明确测试和限制

### 二、TcpServer 接入后的集成测试

重点验证“异步化后，行为与之前一致”。

建议新增或补充以下测试：

- [ ] 单请求仍可正常收发
- [ ] `heartbeat` 路由结果不变
- [ ] `login/register/logout/whoami` 行为不变
- [ ] 无效 JSON 仍能返回错误响应
- [ ] 未登录连接的 `whoami/logout` 仍返回既有错误
- [ ] 连接关闭后异步任务结果不会误发送
- [ ] `weak_ptr.lock()` 失败时结果会被安全丢弃
- [ ] `eventfd` 唤醒后能正确读出计数并完成批量回投

### 三、并发场景测试

建议补充至少一种可重复执行的并发测试：

- [ ] 多个客户端同时发起 `heartbeat`
- [ ] 多个客户端同时发起 `login`
- [ ] 同一连接连续发送多条请求
- [ ] 同一用户多连接重复登录，确认 `SessionManager` 行为符合预期
- [ ] 大量短连接快速建立/关闭时，服务端不会崩溃

这里的关注点不是极限性能，而是：

- 不崩溃
- 不死锁
- 不出现明显错乱响应

### 四、异常与边界测试

- [ ] worker 执行期间连接已关闭
- [ ] `eventfd` 收到多次唤醒时能够批量消费完成队列
- [ ] `eventfd` 在主线程处理中被正确读取，不会造成重复空唤醒
- [ ] 响应为空字符串时不会错误开启 `EPOLLOUT`
- [ ] 停机过程中仍有未完成业务任务
- [ ] 高并发下 `pending_send_` 不会无限增长
- [ ] 半包、多包、超大包行为与当前逻辑一致

## 推荐测试顺序

1. 先完成线程池单测
2. 再完成 `TcpServer` 接入后的基础集成测试
3. 再补并发测试
4. 最后做手工压测和日志观察

## 手工验证建议

除了自动化测试，建议至少做一次手工联调：

- [ ] 启动服务端
- [ ] 用 `client` 连续发送多条请求
- [ ] 观察服务端在并发请求下是否仍可接受新连接
- [ ] 人工验证连接关闭后不会报错刷屏
- [ ] 观察日志中是否出现：
  - 重复关闭
  - 找不到 `conn_id`
  - `epoll_ctl` 修改失败
  - 任务停机时丢失或悬挂
  - `eventfd` 被重复唤醒但完成队列为空

## 验收标准

满足以下条件可认为线程池第一版接入完成：

- [ ] I/O 线程不再直接执行业务处理
- [ ] socket 收发仍只由 `TcpServer` 主线程负责
- [ ] 活跃连接生命周期由 `shared_ptr/weak_ptr` 方案稳定管理
- [ ] 现有认证相关测试全部通过
- [ ] 新增线程池测试通过
- [ ] 并发场景下无崩溃、无死锁、无明显响应异常
- [ ] 停机流程稳定，不出现线程泄漏或资源泄漏

## 第二阶段可选优化

线程池第一版稳定后，可以继续规划：

- [ ] 数据库连接池
- [ ] 区分 I/O 任务与重业务任务的不同线程池
- [ ] 基于消息类型做任务优先级
- [ ] 同连接串行化执行，保证响应顺序
- [ ] 增加任务队列长度限制和过载保护
- [ ] 增加业务耗时统计和线程池监控指标
