# 生产级 MySQL 连接池改造 Todo List

当前项目已经链接 `mysqlclient`，并且 `UserRepository` 已经能通过 `DbConfig` 执行用户表相关操作。但 `DbPool` 目前仍是轻量占位实现，还没有真正承担连接复用、并发借还、健康检查、自动重连和停机回收等职责。

这一阶段的目标是把数据库访问从“每次 Repository 自行处理连接”演进为“业务线程从共享连接池借出 MySQL 连接，使用后自动归还”。

## 目标

- [ ] 连接复用，避免每次请求重复创建和销毁 MySQL 连接
- [ ] 支持多个业务 worker 并发借用连接
- [ ] 支持连接数量上限，避免数据库被打爆
- [ ] 支持连接健康检查和失效连接重建
- [ ] 支持优雅停机，停机后拒绝新借用并等待已借出连接归还
- [ ] 支持 Repository 通过构造函数注入 `DbPool`
- [ ] 支持可观测的错误返回和基础统计信息

## 推荐新增或调整的文件

- [ ] [include/db/db_pool.h](/home/lewis/coding/linux_server/include/db/db_pool.h)
- [ ] [src/db/db_pool.cc](/home/lewis/coding/linux_server/src/db/db_pool.cc)
- [ ] [include/db/db_connection.h](/home/lewis/coding/linux_server/include/db/db_connection.h)
- [ ] [src/db/db_connection.cc](/home/lewis/coding/linux_server/src/db/db_connection.cc)
- [ ] [include/db/db_pool_config.h](/home/lewis/coding/linux_server/include/db/db_pool_config.h)
- [ ] [tests/db_pool_test.cc](/home/lewis/coding/linux_server/tests/db_pool_test.cc)
- [ ] [tests/user_repository_test.cc](/home/lewis/coding/linux_server/tests/user_repository_test.cc)

## 第一阶段：明确连接池接口

- [ ] 定义 `DbConnection`，封装一个底层 `MYSQL*` 连接
- [ ] 定义 `DbConnection` 的基本能力：
  - `connect`
  - `close`
  - `ping`
  - `isAlive`
  - `nativeHandle`
- [ ] 定义 `DbPool::init()`，按配置预创建最小连接数
- [ ] 定义 `DbPool::borrow()`，从池中借出一个连接
- [ ] 定义 `DbPool::stop()`，停止连接池并释放所有空闲连接
- [ ] 明确借连接失败时的返回模型
  - 推荐使用 `std::optional<PooledConnection>` 或结构化结果对象
- [ ] 禁止 Repository 直接持有裸 `MYSQL*`

推荐借出对象使用 RAII 设计，例如：

```cpp
class PooledConnection {
 public:
  PooledConnection(PooledConnection&&) noexcept;
  ~PooledConnection();

  DbConnection* operator->();
  DbConnection& operator*();
};
```

核心要求：

- `PooledConnection` 析构时自动归还连接
- 不允许拷贝，只允许移动
- 归还逻辑必须能处理连接池已经停机的场景

## 第二阶段：连接池配置

- [ ] 在 `DbConfig` 或独立 `DbPoolConfig` 中补充连接池参数
- [ ] 支持最小连接数 `min_connections`
- [ ] 支持最大连接数 `max_connections`
- [ ] 支持借连接等待超时 `borrow_timeout_ms`
- [ ] 支持连接最大空闲时间 `idle_timeout_ms`
- [ ] 支持连接最大存活时间 `max_lifetime_ms`
- [ ] 支持连接失败重试次数 `connect_retry_count`
- [ ] 支持连接失败重试间隔 `connect_retry_delay_ms`
- [ ] 明确默认值，保证本地测试不需要大量配置

建议第一版默认值：

- `min_connections = 1`
- `max_connections = 4`
- `borrow_timeout_ms = 1000`
- `idle_timeout_ms = 300000`
- `max_lifetime_ms = 1800000`
- `connect_retry_count = 1`
- `connect_retry_delay_ms = 100`

## 第三阶段：并发借还与容量控制

- [ ] 使用 `std::mutex` 保护连接池内部状态
- [ ] 使用 `std::condition_variable` 等待可用连接
- [ ] 维护空闲连接队列
- [ ] 维护当前总连接数
- [ ] `borrow()` 优先复用空闲连接
- [ ] 空闲连接不足且未达到上限时创建新连接
- [ ] 达到上限时等待归还或超时
- [ ] 停机后 `borrow()` 必须立即失败
- [ ] 归还连接时唤醒等待中的业务线程

锁边界要求：

- 不在持有池锁时执行慢 SQL
- 不在持有池锁时执行业务逻辑
- 创建新连接可以先预留连接名额，再在锁外完成真实建连
- 归还连接时只做轻量状态更新和队列操作

## 第四阶段：健康检查与自动重连

- [ ] 借出前检查连接是否仍然可用
- [ ] 使用 `mysql_ping` 或等价接口做轻量健康检查
- [ ] 发现连接失效时丢弃旧连接并尝试创建新连接
- [ ] SQL 执行出现连接断开错误时，Repository 不应盲目复用该连接
- [ ] 区分“查询失败”和“连接失效”
- [ ] 对失效连接计数，便于日志和排查
- [ ] 避免无限重连，重试次数必须有上限

建议第一版策略：

- 借出连接前做一次 `ping`
- `ping` 失败则关闭并重建
- 重建失败则减少连接计数并返回借用失败
- 不在同一次 Repository 操作中自动重复执行写 SQL，避免重复写入风险

## 第五阶段：Repository 接入连接池

- [ ] 修改 `UserRepository` 构造函数，支持注入 `DbPool&`
- [ ] 保留测试替身接口 `IUserRepository`
- [ ] `findByUsername` 从连接池借连接后执行查询
- [ ] `findById` 从连接池借连接后执行查询
- [ ] `createUser` 从连接池借连接后执行插入
- [ ] 每个 Repository 方法只在方法作用域内持有 `PooledConnection`
- [ ] Repository 不缓存借出的连接
- [ ] Service 层不感知连接池细节
- [ ] `MessageHandler` 或应用启动层负责创建共享 `DbPool`

建议依赖方向：

```text
main_runner
  -> DbPool
  -> UserRepository(DbPool&)
  -> UserService(IUserRepository&)
  -> MessageHandler
  -> TcpServer
```

## 第六阶段：停机与资源回收

- [ ] `DbPool::stop()` 设置 stopping 标志
- [ ] 停机后拒绝新的 `borrow()`
- [ ] 唤醒所有等待连接的线程
- [ ] 关闭所有空闲连接
- [ ] 等待或记录仍未归还的连接
- [ ] 明确连接泄漏检测策略
- [ ] `DbPool` 析构时调用 `stop()`
- [ ] `TcpServer` 停机时先停止接收新业务，再等待 worker 结束，最后释放 `DbPool`

推荐原则：

- 连接池生命周期应长于 `UserRepository`
- 连接池生命周期应覆盖所有 worker 任务
- 不要在 worker 仍可能访问数据库时销毁 `DbPool`

## 第七阶段：错误模型与日志

- [ ] 为连接池定义内部错误类型
- [ ] 区分配置错误、连接失败、借用超时、健康检查失败、SQL 执行失败
- [ ] Repository 将连接池错误映射到 `RepositoryStatus::kQueryFailed` 或更细粒度状态
- [ ] 日志中包含错误码、MySQL 错误信息和连接池统计
- [ ] 避免在日志中输出数据库密码

建议至少记录：

- 当前总连接数
- 当前空闲连接数
- 当前等待借连接的线程数
- 累计创建连接数
- 累计重连次数
- 累计借用超时次数

## 第八阶段：测试计划

- [ ] `DbPool` 初始化成功测试
- [ ] 配置缺失时初始化失败测试
- [ ] 借出连接后归还测试
- [ ] 多次借还复用同一连接测试
- [ ] 最大连接数限制测试
- [ ] 达到上限后等待归还测试
- [ ] 借连接超时测试
- [ ] `stop()` 后拒绝借连接测试
- [ ] 重复调用 `stop()` 幂等测试
- [ ] 健康检查失败后重建连接测试
- [ ] 多 worker 并发借还压力测试
- [ ] `UserRepository` 通过连接池完成查询和插入测试

测试建议分两类：

- 不依赖真实 MySQL 的单元测试：使用 fake `DbConnection` 或可注入连接工厂
- 依赖真实 MySQL 的集成测试：通过环境变量控制是否启用

## 验收标准

- [ ] `DbPool` 支持并发安全借还连接
- [ ] 连接数量不会超过配置上限
- [ ] 借出对象析构后能自动归还连接
- [ ] 连接失效后不会继续放回可用队列
- [ ] 停机后不会有 worker 继续使用已销毁连接池
- [ ] `UserRepository` 不再直接管理底层连接生命周期
- [ ] 现有认证和用户相关测试全部通过
- [ ] 新增连接池单元测试和集成测试通过
- [ ] 日志不会泄漏数据库密码

## 暂不建议第一版实现的能力

- [ ] SQL 级自动重试写操作
- [ ] 读写分离
- [ ] 多数据库实例负载均衡
- [ ] 连接池动态扩缩容复杂策略
- [ ] 后台定时扫描线程
- [ ] 完整 Prometheus 指标导出

第一版重点应放在：RAII 借还、容量上限、超时等待、健康检查、停机安全、Repository 注入。
