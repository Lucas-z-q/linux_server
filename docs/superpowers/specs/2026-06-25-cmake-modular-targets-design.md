# CMake 模块化目标设计

## 目标

将当前直接把大量实现文件挂到 `server` 和各测试目标上的构建方式，改为按职责划分的静态库。主程序和测试只声明直接依赖，避免重复编译源码，并让构建依赖反映代码架构。

## 目标结构

根目录 `CMakeLists.txt` 只负责项目级设置、依赖发现和子目录装配：

```text
CMakeLists.txt
cmake/Dependencies.cmake
src/CMakeLists.txt
client/CMakeLists.txt
tests/CMakeLists.txt
```

源码拆分为以下目标：

| Target | 职责 | 主要源码 |
| --- | --- | --- |
| `chat_common` | 日志、校验和密码哈希等公共运行时 | `common/*`、`security/*` |
| `chat_codec` | TCP 分包、JSON 编解码和协议辅助 | `codec/*`、`protocol/*` |
| `chat_net` | 连接、epoll 服务端和线程池 | `TcpServer.cc`、`TcpConnection.cc`、`ConnectionContext.cc`、`EchoHandler.cc`、`concurrency/*` |
| `chat_db` | MySQL 连接、连接池和仓储实现 | `db/*` |
| `chat_redis` | Redis 客户端、缓存、全局会话和推送流 | `redis/*`、`cache/*`、`stream/*`、`server/redis_session_store.cc`、`config/redis_config.cc` |
| `chat_service` | 本地会话管理和业务服务 | `server/session_manager.cc`、`service/*` |
| `chat_handler` | 协议请求路由和响应编排 | `handler/message_handler.cc` |
| `chat_server_app` | 配置加载和服务启动辅助 | `config/config_loader.cc`、`app/main_runner.cc` |

`server` 仅编译 `src/main.cc`，通过链接上述库完成装配。`client` 只链接 `chat_codec`。

## 依赖方向

依赖保持单向：

```text
chat_common
  -> chat_codec
  -> chat_net

chat_common
  -> chat_db
  -> chat_redis
  -> chat_service
  -> chat_handler

chat_common
  -> chat_server_app

server
  -> chat_server_app
  -> chat_handler
  -> chat_net
  -> chat_db
  -> chat_redis
```

`chat_service` 同时链接 `chat_db` 和 `chat_redis`，因为当前公开服务接口直接使用仓储、限流、全局会话和远端推送抽象。此次只整理构建边界，不重写 C++ 接口。

## 测试目标

`tests/CMakeLists.txt` 提供统一的测试目标辅助函数，集中设置：

- 测试源码路径
- `include`、`third_party` 和 `tests` 头文件目录
- `add_test`
- 可选的 GoogleTest 依赖
- 可选的 timeout、label 和 skip return code

每个测试只链接需要的库。例如：

- codec 测试链接 `chat_codec`
- service 测试链接 `chat_service`
- handler 测试链接 `chat_handler`
- network 测试链接 `chat_net`
- repository 测试链接 `chat_db`
- Redis 测试链接 `chat_redis`

测试替身继续作为测试源码或头文件使用，不进入生产库。

## 兼容性

仓库内部统一使用新的 `chat_*` 名称。保留以下 CMake alias，降低已有本地脚本立即失效的风险：

- `chat_logger` -> `chat_common`
- `security_runtime` -> `chat_common`
- `redis_runtime` -> `chat_redis`

不修改可执行文件名称、CTest 测试名称、协议、运行时行为或配置格式。

## 验证

重构完成后执行：

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
mise run fix-md
mise run check-md
```

当前环境的 `server_integration_test` 固定绑定 `127.0.0.1:8080`，而该端口在重构前已被外部进程占用。最终验证需要单独记录该环境失败，并确认其他测试结果没有新增回归。
