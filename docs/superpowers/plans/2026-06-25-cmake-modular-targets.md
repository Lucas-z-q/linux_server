# CMake 模块化目标实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将服务端和测试重复列举源码的构建方式重构为八个职责清晰的静态库。

**Architecture:** 根 CMake 只保留项目设置和子目录装配，第三方依赖集中在 `cmake/Dependencies.cmake`。生产源码、客户端和测试分别由子目录 CMake 管理，测试通过统一辅助函数链接所需库。

**Tech Stack:** CMake 3.10、C++17、CTest、GoogleTest、MySQL client、hiredis、Threads

---

### Task 1: 提取项目依赖

**Files:**

- Create: `cmake/Dependencies.cmake`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: 移动依赖发现逻辑**

将 Threads、crypt、GoogleTest、hiredis 和 MySQL client 的发现逻辑移动到 `cmake/Dependencies.cmake`，并为 GoogleTest 的 FetchContent URL 设置 `DOWNLOAD_EXTRACT_TIMESTAMP TRUE`。

- [ ] **Step 2: 缩短根构建入口**

根 `CMakeLists.txt` 保留 C++17 设置、`enable_testing()`、依赖模块加载和以下子目录：

```cmake
add_subdirectory(src)
add_subdirectory(client)
add_subdirectory(tests)
```

- [ ] **Step 3: 配置验证**

Run:

```bash
cmake -S . -B build
```

Expected: CMake 配置和生成成功，不再出现 GoogleTest 的 `DOWNLOAD_EXTRACT_TIMESTAMP` 警告。

### Task 2: 建立生产库目标

**Files:**

- Create: `src/CMakeLists.txt`

- [ ] **Step 1: 定义公共 target 设置函数**

创建内部辅助函数，为每个 `chat_*` 静态库设置 `${PROJECT_SOURCE_DIR}/include` 公共头文件目录。

- [ ] **Step 2: 定义八个静态库**

按设计文档中的源码归属创建：

```cmake
chat_common
chat_codec
chat_net
chat_db
chat_redis
chat_service
chat_handler
chat_server_app
```

通过 `target_link_libraries` 声明库之间以及 Threads、crypt、MySQL client、hiredis 的依赖。

- [ ] **Step 3: 保留旧 target alias**

添加：

```cmake
add_library(chat_logger ALIAS chat_common)
add_library(security_runtime ALIAS chat_common)
add_library(redis_runtime ALIAS chat_redis)
```

- [ ] **Step 4: 精简 server**

`server` 只编译 `main.cc`，链接应用、handler、network、database 和 Redis 目标。

- [ ] **Step 5: 构建生产目标**

Run:

```bash
cmake --build build --target server -j2
```

Expected: `server` 构建成功。

### Task 3: 独立客户端构建

**Files:**

- Create: `client/CMakeLists.txt`

- [ ] **Step 1: 定义 client**

`client` 只编译 `client.cc` 并链接 `chat_codec`。

- [ ] **Step 2: 构建客户端**

Run:

```bash
cmake --build build --target client -j2
```

Expected: `client` 构建成功。

### Task 4: 将测试迁移到库依赖

**Files:**

- Create: `tests/CMakeLists.txt`

- [ ] **Step 1: 建立普通测试辅助函数**

辅助函数接收 target、源码和链接库，统一调用 `add_executable`、设置测试头文件目录并注册 `add_test`。

- [ ] **Step 2: 建立 GoogleTest 辅助函数**

在普通辅助函数基础上统一链接：

```cmake
GTest::gtest
GTest::gtest_main
```

- [ ] **Step 3: 迁移所有测试**

保留现有 target 名和 CTest 名。删除生产 `.cc` 的重复列举，改为链接 `chat_codec`、`chat_net`、`chat_db`、`chat_redis`、`chat_service`、`chat_handler` 或 `chat_server_app`。

- [ ] **Step 4: 保留集成测试属性**

保留 binary path compile definition、target dependency、timeout、label 和 `SKIP_RETURN_CODE 77`。

- [ ] **Step 5: 构建全部目标**

Run:

```bash
cmake --build build -j2
```

Expected: 所有目标构建成功。

### Task 5: 更新架构文档并验证

**Files:**

- Modify: `docs/project_structure.md`

- [ ] **Step 1: 更新构建目标说明**

将旧的运行时库说明替换为八个 `chat_*` 库及其依赖职责。

- [ ] **Step 2: 格式化 Markdown**

Run:

```bash
mise run fix-md
mise run check-md
```

Expected: Markdown 检查通过。

- [ ] **Step 3: 全量验证**

Run:

```bash
cmake -S . -B build
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Expected: 配置和构建成功；除已记录的 8080 端口占用外不新增测试失败。

- [ ] **Step 4: 检查源码重复编译**

Run:

```bash
rg -n 'src/.+\.cc' tests/CMakeLists.txt
```

Expected: 无生产源码路径；测试目标仅列测试自身源码。

- [ ] **Step 5: 检查工作区**

Run:

```bash
git diff --check
git status --short
```

Expected: 无 whitespace 错误，变更仅包含本次 CMake 重构和文档。
