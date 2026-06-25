# Task Marker and Comment Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove stale project-owned task markers and misleading comments, delete unused placeholder code, and move the remaining concrete engineering work into an acceptance-driven roadmap.

**Architecture:** This is a behavior-preserving cleanup. Source comments will describe current ownership, threading, protocol, and persistence behavior; unresolved engineering work will live only in `docs/roadmap.md`. Unused placeholder translation units and headers will be removed from both the filesystem and CMake targets.

**Tech Stack:** C++17, CMake, GoogleTest/CTest, Markdown, ripgrep, git

---

## File map

- Create `docs/roadmap.md`: centralized engineering roadmap with explicit acceptance criteria.
- Delete `include/EchoHandler.h` and `src/EchoHandler.cc`: obsolete echo example no longer used by the production entry point or tests.
- Delete `include/server/connection_manager.h`: empty placeholder type with no caller.
- Delete `include/protocol/protocol_helper.h` and `src/protocol/protocol_helper.cc`: unused response helper module.
- Modify `src/CMakeLists.txt`: remove deleted source files from `chat_codec` and `chat_net`.
- Modify `src/handler/message_handler.cc`: remove the unused protocol-helper include.
- Modify project headers under `include/`: remove completed or speculative markers and correct stale module descriptions.
- Modify `src/TcpServer.cc`: move the remaining disconnect-side-effect concern to the roadmap.
- Modify `README.md` and `docs/project_structure.md`: replace “reserved” and open-ended future wording with current behavior and roadmap links.

No production behavior, public protocol field, database schema, or test expectation should change.

### Task 1: Create the centralized roadmap

**Files:**

- Create: `docs/roadmap.md`

- [ ] **Step 1: Create the roadmap with only accepted work**

Write the following structure:

```markdown
# 工程路线图

## 维护规则

- 源码注释只描述当前行为、约束和设计意图。
- 未完成工作必须写明范围和验收条件。
- 没有明确价值或验收标准的设想不进入路线图。

## 信号驱动的优雅退出

为生产入口安装 `SIGINT` 和 `SIGTERM` 处理机制，安全唤醒服务器事件循环并复用现有停机路径。

验收条件：

- 收到退出信号后停止接受新连接。
- 停止 Redis Push Stream 等外部事件源。
- 等待已提交的 worker 清理任务完成。
- 进程正常退出，并有自动化测试覆盖。

## 请求执行期间断连的副作用语义

明确 worker 执行期间连接断开后，数据库写入、限流和 Redis 操作是否继续，以及响应阶段如何抑制失效连接的会话副作用。

验收条件：

- 文档定义允许完成和必须抑制的副作用边界。
- 登录和发送消息流程具有断连并发测试。
- `IMessageHandler` 与 `TcpServer` 的注释和实现一致。

## MessageHandler 按业务域拆分

当路由和依赖继续增长时，将认证、聊天、好友和群组处理拆分为独立组件。

验收条件：

- 顶层 handler 只负责信封解析、认证前置校验和路由。
- 子 handler 可独立测试。
- 对外协议与响应格式保持兼容。

## 请求可观测性

增加不包含敏感数据的结构化请求日志和处理耗时。

验收条件：

- 稳定记录消息类型、连接 ID、结果码和耗时。
- 不记录密码、token 或消息正文。
- 成功、参数错误和内部错误路径均可通过测试日志 sink 验证。

## 统一错误文案

为对外错误码建立默认文案映射。

验收条件：

- 每个对外 `ErrorCode` 都有默认文案。
- 允许业务上下文覆盖，但不泄露数据库错误细节。
- 协议测试验证错误码和默认文案稳定性。

## 连接状态迁移说明

记录连接建立、活跃、半关闭、关闭中和已关闭的状态迁移。

验收条件：

- 文档列出状态、触发事件和资源所有权。
- 关闭路径与 `ConnectionMeta::State` 一致。
- 覆盖重复关闭和停机期间关闭。

## 跨节点多端登录

将 Redis 用户级单 presence 扩展为设备或连接维度的多 presence。

验收条件：

- 明确 `device_id` 或等价稳定标识。
- 同一用户可跨节点保持多个在线端。
- 登出和断连只清理对应在线端。
- 推送路由覆盖全部目标在线端。

## 时间字段模型统一

统一数据库时间、协议时间和运行时单调时间的类型与单位。

验收条件：

- 明确 wall clock 与 monotonic clock 的使用边界。
- 协议时间戳使用统一单位。
- Repository 不再用无类型字符串保存需要计算的时间。
```

- [ ] **Step 2: Check roadmap formatting**

Run:

```bash
git diff --check -- docs/roadmap.md
```

Expected: exit code `0` with no output.

- [ ] **Step 3: Commit the roadmap**

```bash
git add docs/roadmap.md
git commit -m "docs: add engineering roadmap"
```

### Task 2: Remove unused placeholder modules

**Files:**

- Delete: `include/EchoHandler.h`
- Delete: `src/EchoHandler.cc`
- Delete: `include/server/connection_manager.h`
- Delete: `include/protocol/protocol_helper.h`
- Delete: `src/protocol/protocol_helper.cc`
- Modify: `src/CMakeLists.txt`
- Modify: `src/handler/message_handler.cc`

- [ ] **Step 1: Verify the modules have no live callers**

Run:

```bash
rg -n 'EchoHandler|ConnectionManager|protocol_helper|makeResponseType|makeSuccessResponse|makeErrorResponse|isAuthMessage' \
  CMakeLists.txt src include tests
```

Expected: references are limited to the five files being deleted, `src/CMakeLists.txt`, and the unused include in `src/handler/message_handler.cc`.

- [ ] **Step 2: Remove deleted sources from CMake**

Change the relevant library declarations to:

```cmake
add_chat_library(chat_codec
    codec/json_codec.cc
    codec/packet_codec.cc
)

add_chat_library(chat_net
    ConnectionContext.cc
    TcpConnection.cc
    TcpServer.cc
    concurrency/thread_pool.cc
)
```

- [ ] **Step 3: Remove the unused include**

Delete this line from `src/handler/message_handler.cc`:

```cpp
#include "protocol/protocol_helper.h"
```

- [ ] **Step 4: Delete the obsolete files**

Delete exactly:

```text
include/EchoHandler.h
src/EchoHandler.cc
include/server/connection_manager.h
include/protocol/protocol_helper.h
src/protocol/protocol_helper.cc
```

- [ ] **Step 5: Build the affected libraries**

Run:

```bash
cmake -S . -B build
cmake --build build --target chat_codec chat_net chat_handler
```

Expected: all three targets build successfully.

- [ ] **Step 6: Commit the placeholder removal**

```bash
git add src/CMakeLists.txt src/handler/message_handler.cc
git add -u include src
git commit -m "refactor: remove unused placeholder modules"
```

### Task 3: Correct network and handler comments

**Files:**

- Modify: `include/net/TcpServer.h`
- Modify: `src/TcpServer.cc`
- Modify: `include/net/IMessageHandler.h`
- Modify: `include/net/TcpConnection.h`
- Modify: `include/net/ConnectionMeta.h`
- Modify: `include/handler/message_handler.h`
- Modify: `include/server/session_manager.h`
- Modify: `include/service/user_service.h`
- Modify: `include/model/connection_session.h`

- [ ] **Step 1: Make `TcpServer` comments describe current behavior**

Remove the completed marker lines above `TcpServer`. Replace the worker-pool member comment with:

```cpp
// 执行业务处理、持久化调用和连接关闭后的业务清理。
ThreadPool worker_pool_;
```

Delete the worker-lambda marker in `src/TcpServer.cc`; its unresolved concern is now covered by the roadmap.

- [ ] **Step 2: Correct `IMessageHandler` threading and exception semantics**

Replace the stale concurrency block with:

```cpp
// 【并发与生命周期语义】
// 1. handle() 在 worker 线程执行，可以调用持久化和外部服务，但不能直接操作 socket。
// 2. 本地连接会话的绑定和解绑通过 HandleResult 延后到 I/O 线程执行。
// 3. TcpServer 在应用延后副作用前重新校验连接是否存活；失效连接的响应和会话动作会被丢弃。
// 4. handle() 抛出的异常由 TcpServer 捕获，并转换为关闭连接的响应任务。
```

Replace the `onConnectionClosed()` comment with:

```cpp
// 连接关闭后清理业务状态。TcpServer 将该回调投递到 worker pool，
// 避免 Redis 或数据库清理阻塞 I/O 线程。
```

- [ ] **Step 3: Remove completed network markers**

Remove the completed marker blocks from:

```text
include/net/TcpConnection.h
include/net/ConnectionMeta.h
include/server/session_manager.h
include/model/connection_session.h
```

Keep the existing field and method comments that describe current behavior. Do not add speculative replacement comments.

- [ ] **Step 4: Remove handler and service wish-list markers**

Remove the marker blocks from:

```text
include/handler/message_handler.h
include/service/user_service.h
```

The accepted handler split and observability work now belongs to `docs/roadmap.md`; the already implemented worker model needs no replacement marker.

- [ ] **Step 5: Verify this file group is clean**

Run:

```bash
rg -n 'T[D]O|FIXM[E]|X[X]X' \
  include/net include/handler include/server include/service include/model/connection_session.h src/TcpServer.cc
```

Expected: no output.

- [ ] **Step 6: Commit network and handler comment corrections**

```bash
git add include/net include/handler include/server include/service include/model/connection_session.h src/TcpServer.cc
git commit -m "docs: correct network and handler comments"
```

### Task 4: Clean model, configuration, database, codec, and protocol comments

**Files:**

- Modify: `include/model/user_record.h`
- Modify: `include/config/db_config.h`
- Modify: `include/db/db_pool.h`
- Modify: `include/db/user_repository.h`
- Modify: `include/codec/json_codec.h`
- Modify: `include/codec/packet_codec.h`
- Modify: `include/common/error_code.h`
- Modify: `include/common/message.h`
- Modify: `include/common/response.h`
- Modify: `include/common/types.h`
- Modify: `include/protocol/auth_messages.h`
- Modify: `include/protocol/chat_messages.h`

- [ ] **Step 1: Correct current module descriptions**

Use these descriptions:

```cpp
// DbConfig 描述单个 MySQL 连接和连接池创建连接时使用的基础参数。
```

```cpp
// DbPool 管理 MySQL C API 连接的创建、健康检查、借出、归还和销毁。
// PooledConnection 通过 RAII 将可复用连接归还连接池。
```

```cpp
// PacketCodec 使用换行符分隔应用层消息，并限制单个消息的最大缓存大小。
```

In `UserRecord`, correct the status field comment to:

```cpp
// 用户状态：1 表示启用，0 表示禁用。
int status = 0;
```

- [ ] **Step 2: Remove completed and speculative markers**

Remove the marker blocks from every file listed in this task. Preserve useful field constraints and current protocol descriptions.

Do not replace removed markers with plans for:

- phone or email login;
- refresh-token or password-change messages;
- file messages;
- trace IDs;
- JSON schema frameworks;
- JSON object key ordering;
- error-code range refactoring;
- length-prefixed framing.

These items are not part of the accepted roadmap.

- [ ] **Step 3: Verify this file group is clean**

Run:

```bash
rg -n 'T[D]O|FIXM[E]|X[X]X' \
  include/model/user_record.h include/config include/db include/codec include/common include/protocol
```

Expected: no output.

- [ ] **Step 4: Compile all production libraries**

Run:

```bash
cmake --build build
```

Expected: build completes successfully.

- [ ] **Step 5: Commit model and protocol comment cleanup**

```bash
git add include/model/user_record.h include/config include/db include/codec include/common include/protocol
git commit -m "docs: remove stale model and protocol markers"
```

### Task 5: Align public documentation with current behavior

**Files:**

- Modify: `README.md`
- Modify: `docs/project_structure.md`

- [ ] **Step 1: Correct repeated-login documentation**

Change the `USER_ALREADY_ONLINE` table entry to:

```markdown
| `1005` | `USER_ALREADY_ONLINE` | 兼容保留；当前重复登录会签发新 token 并撤销旧 token |
```

- [ ] **Step 2: Remove open-ended protocol migration wording**

Replace the README boundary line with:

```markdown
- 应用层当前使用换行符分隔消息，并限制单包大小。
```

- [ ] **Step 3: Link cross-node multi-device work to the roadmap**

Replace the final paragraph in the “Session 与多端” section with:

```markdown
当前多端策略是本地连接级多端：不引入显式 `device_id`，每条已认证连接视作一个在线端。跨节点 Redis presence 仍是用户级单 presence，完整跨节点多端能力见 [工程路线图](roadmap.md)。
```

- [ ] **Step 4: Check documentation formatting**

Run:

```bash
git diff --check -- README.md docs/project_structure.md docs/roadmap.md
```

Expected: exit code `0` with no output.

- [ ] **Step 5: Commit documentation alignment**

```bash
git add README.md docs/project_structure.md
git commit -m "docs: align project boundaries with roadmap"
```

### Task 6: Run repository-wide verification

**Files:**

- Verify all modified and deleted files.

- [ ] **Step 1: Verify no project-owned markers remain**

Run:

```bash
rg -n --hidden \
  --glob '!third_party/**' \
  --glob '!build/**' \
  --glob '!.git/**' \
  'T[D]O|FIXM[E]|X[X]X' .
```

Expected: no output and ripgrep exit code `1`, meaning no match.

- [ ] **Step 2: Verify deleted symbols and files are not referenced**

Run:

```bash
rg -n 'EchoHandler|ConnectionManager|protocol_helper|makeResponseType|makeSuccessResponse|makeErrorResponse|isAuthMessage' \
  CMakeLists.txt src include tests
```

Expected: no output.

- [ ] **Step 3: Format C++ sources**

Run:

```bash
clang-format -i $(git ls-files '*.cc' '*.h')
```

Expected: command succeeds. Review the diff to ensure only intended formatting changes occurred.

- [ ] **Step 4: Run the full build and test suite**

Run:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: configure and build succeed; all registered tests pass or explicitly configured external-dependency tests skip according to existing behavior.

- [ ] **Step 5: Run Markdown verification**

Run:

```bash
mise run fix-md
mise run check-md
```

Expected: both commands succeed with zero Markdown errors. If `mise` is unavailable in the environment, record that limitation and retain successful `git diff --check` evidence.

- [ ] **Step 6: Review the final diff**

Run:

```bash
git status --short
git diff --check
git diff --stat
git diff
```

Expected: only the planned source, CMake, README, roadmap, and project-structure changes are present.

- [ ] **Step 7: Commit final mechanical fixes if verification changed files**

If formatting or Markdown repair changed tracked files:

```bash
git add README.md docs include src
git commit -m "style: normalize cleanup documentation"
```

If verification produced no changes, skip this commit.
