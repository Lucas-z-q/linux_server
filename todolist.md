# Linux Chat Server TODO List

本文档用于指导当前项目逐步实现“基于 TCP 的注册/登录能力”。

目标不是一次性做完所有功能，而是按阶段推进：

- 每阶段都有清晰的实现目标
- 每阶段都尽量可单独验证
- 先打通主链路，再补数据库、会话和错误处理细节

---

## 第一阶段：整理工程结构并保证可编译

### 目标

先把目录结构、头文件职责、源文件骨架整理好，保证后续写逻辑时不需要频繁返工。

### 实现思路

这一阶段不追求业务可用，重点是“架子先搭稳”。

建议检查并确认下面几件事：

- `common/` 只放公共类型和响应结构
- `codec/` 负责 JSON 编解码和 TCP 分包
- `protocol/` 放认证相关消息结构
- `db/` 放数据库访问接口
- `service/` 放注册/登录业务逻辑
- `handler/` 负责消息分发
- `net/` 保持现有 TCP 网络层

建议确认的关键文件：

- [include/common/message.h](/home/lzq/coding/linux_server/include/common/message.h)
- [include/common/response.h](/home/lzq/coding/linux_server/include/common/response.h)
- [include/protocol/auth_messages.h](/home/lzq/coding/linux_server/include/protocol/auth_messages.h)
- [include/handler/message_handler.h](/home/lzq/coding/linux_server/include/handler/message_handler.h)
- [include/service/user_service.h](/home/lzq/coding/linux_server/include/service/user_service.h)
- [include/db/user_repository.h](/home/lzq/coding/linux_server/include/db/user_repository.h)

### 这一阶段怎么测试

1. 执行项目编译，确认工程结构没有破坏现有构建

```bash
cmake --build build
```

2. 确认新增头文件和 `.cc` 文件都已经纳入项目管理

```bash
find include src -maxdepth 3 -type f | sort
```

3. 确认旧的 echo 服务还能启动

```bash
./build/server
```

### 完成标志

- 工程可以正常编译
- 新目录结构已经稳定
- 后续可以开始填认证逻辑

---

## 第二阶段：打通 JSON 协议编解码

### 目标

让服务端能够把一段原始 JSON 请求解析成内部消息对象，再把响应对象编码回 JSON 字符串。

### 实现思路

优先完成 `JsonCodec`：

- 实现 `decodeMessage`
- 实现 `encodeResponse`
- 实现 `parseRegisterRequest`
- 实现 `parseLoginRequest`
- 实现响应数据填充函数

建议只处理这几种消息：

- `register`
- `login`
- `logout`
- `heartbeat`

当前阶段不要急着接 MySQL，先只保证“协议解析对，错误返回稳”。

建议重点处理：

- `msg_type` 缺失
- `seq` 不是数字
- `data` 不是对象
- `username/password` 缺失
- 非法 JSON

涉及文件：

- [include/codec/json_codec.h](/home/lzq/coding/linux_server/include/codec/json_codec.h)
- [src/codec/json_codec.cc](/home/lzq/coding/linux_server/src/codec/json_codec.cc)
- [include/protocol/protocol_helper.h](/home/lzq/coding/linux_server/include/protocol/protocol_helper.h)
- [src/protocol/protocol_helper.cc](/home/lzq/coding/linux_server/src/protocol/protocol_helper.cc)

### 这一阶段怎么测试

1. 编译通过

```bash
cmake --build build
```

2. 手工构造几段 JSON，验证解析函数是否返回预期结果

建议临时写最小测试代码，或者先在 `main` 里短暂调用：

- 合法 `login`
- 缺少 `password`
- `seq` 是字符串
- 非法 JSON

3. 观察响应 JSON 是否统一包含：

- `msg_type`
- `seq`
- `code`
- `message`
- `data`

### 完成标志

- 正常请求能解析
- 异常请求能稳定返回错误
- 协议格式不再反复改动

---

## 第三阶段：打通 MessageHandler 路由

### 目标

让服务端能按 `msg_type` 分发请求，并生成对应响应。

### 实现思路

这个阶段的重点是把请求链路接起来：

`原始请求 -> JsonCodec -> MessageHandler -> Response -> JSON字符串`

建议先把下面几种处理写完整：

- `handleRegister`
- `handleLogin`
- `handleLogout`
- `handleHeartbeat`

此时可以先不接真实数据库，先用占位逻辑返回：

- 注册：返回“未实现”
- 登录：返回“未实现”
- 心跳：返回当前时间

这样可以先验证整个 handler 分发链没问题。

涉及文件：

- [include/handler/message_handler.h](/home/lzq/coding/linux_server/include/handler/message_handler.h)
- [src/handler/message_handler.cc](/home/lzq/coding/linux_server/src/handler/message_handler.cc)

### 这一阶段怎么测试

1. 直接在本地运行服务端

```bash
./build/server
```

2. 另开终端，用 `nc` 或自写 client 发 JSON 请求

```bash
printf '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}\n' | nc 127.0.0.1 8080
```

```bash
printf '{"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}}\n' | nc 127.0.0.1 8080
```

3. 验证未知消息类型是否返回统一错误

```bash
printf '{"msg_type":"unknown","seq":3,"token":"","data":{}}\n' | nc 127.0.0.1 8080
```

### 完成标志

- 服务端能识别不同 `msg_type`
- 心跳能返回正确响应
- 非法消息和未知消息有统一错误返回

---

## 第四阶段：设计 MySQL 用户表

### 目标

为注册/登录准备最小可用的数据表结构。

### 实现思路

建议先只建一张 `users` 表，字段尽量少而够用：

- `id`
- `username`
- `password_hash`
- `nickname`
- `status`
- `created_at`
- `updated_at`

建议注意：

- `username` 要唯一索引
- 不存明文密码
- 先不引入太多可选字段，比如手机号、邮箱

推荐你自己先写好建表 SQL，并明确：

- `status` 含义
- 用户名最大长度
- 字符集

### 这一阶段怎么测试

1. 在 MySQL 中执行建表 SQL

2. 检查表结构

```sql
DESC users;
SHOW INDEX FROM users;
```

3. 手动插入一条测试用户

```sql
INSERT INTO users(username, password_hash, nickname, status)
VALUES('alice', 'hash_xxx', 'Alice', 0);
```

4. 手动执行查询，确认唯一索引生效

```sql
SELECT * FROM users WHERE username = 'alice';
```

### 完成标志

- `users` 表创建完成
- 唯一索引正确
- 手工插入和查询正常

---

## 第五阶段：实现 UserRepository

### 目标

把对 `users` 表的访问封装起来，让业务层不直接写 SQL。

### 实现思路

建议先实现这几个核心方法：

- `findByUsername`
- `findById`
- `createUser`

这一层只负责数据访问，不负责：

- 密码正确性判断
- 是否允许重复登录
- token 生成

Repository 的职责应该很单纯：

- 读库
- 写库
- 映射结果到 `UserRecord`

涉及文件：

- [include/db/user_repository.h](/home/lzq/coding/linux_server/include/db/user_repository.h)
- [src/db/user_repository.cc](/home/lzq/coding/linux_server/src/db/user_repository.cc)
- [include/model/user_record.h](/home/lzq/coding/linux_server/include/model/user_record.h)

### 这一阶段怎么测试

1. 写最小查询测试，查一个已存在用户

2. 测试查不存在的用户名，确认返回空结果

3. 测试插入新用户

4. 测试重复用户名插入，确认能识别失败

建议测试场景：

- 正常查到用户
- 查不到用户
- 正常插入
- 插入重复用户名

### 完成标志

- 可以通过 `UserRepository` 稳定查用户和插用户
- 不需要在 service 里手写 SQL

---

## 第六阶段：实现 UserService 注册流程

### 目标

真正实现注册逻辑。

### 实现思路

建议流程：

1. 校验 `username/password`
2. 调 `UserRepository::findByUsername`
3. 若已存在，返回“用户名已存在”
4. 对密码做哈希
5. 调 `createUser`
6. 返回 `user_id`

这里建议先把密码哈希做成单独函数，即使第一版先简单处理，也别直接明文入库。

涉及文件：

- [include/service/user_service.h](/home/lzq/coding/linux_server/include/service/user_service.h)
- [src/service/user_service.cc](/home/lzq/coding/linux_server/src/service/user_service.cc)

### 这一阶段怎么测试

1. 调用注册接口，传一个新用户名

预期：

- 返回成功
- MySQL 中新增一条记录

2. 再次注册同用户名

预期：

- 返回“用户名已存在”
- 数据库中不会新增重复记录

3. 传空用户名或空密码

预期：

- 返回参数错误

建议请求示例：

```bash
printf '{"msg_type":"register","seq":1,"token":"","data":{"username":"alice","password":"123456","nickname":"Alice"}}\n' | nc 127.0.0.1 8080
```

### 完成标志

- 注册成功能写库
- 重复用户名能正确拦截
- 参数校验生效

---

## 第七阶段：实现 UserService 登录流程

### 目标

真正实现“登录前查 MySQL 用户信息”的主链路。

### 实现思路

建议登录流程固定成下面这样：

1. 校验 `username/password`
2. 根据 `username` 查 MySQL 用户表
3. 如果查不到，返回登录失败
4. 取出数据库中的 `password_hash`
5. 对用户输入密码做相同哈希
6. 比较哈希结果
7. 成功则返回 `user_id/nickname/token`

注意：

- 登录失败建议统一返回“用户名或密码错误”
- 不要把“用户不存在”和“密码错误”分开提示给客户端

涉及文件：

- [src/service/user_service.cc](/home/lzq/coding/linux_server/src/service/user_service.cc)
- [src/db/user_repository.cc](/home/lzq/coding/linux_server/src/db/user_repository.cc)

### 这一阶段怎么测试

1. 用正确用户名密码登录

预期：

- 返回成功
- 返回 `user_id`
- 返回 `token`

2. 用户名不存在

预期：

- 返回失败

3. 密码错误

预期：

- 返回失败

4. 空用户名或空密码

预期：

- 返回参数错误

测试示例：

```bash
printf '{"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}}\n' | nc 127.0.0.1 8080
```

### 完成标志

- 登录前已经真正查询 MySQL
- 正确密码能登录
- 错误密码不能登录

---

## 第八阶段：维护连接登录态

### 目标

登录成功后，服务端知道“这个 TCP 连接属于哪个用户”。

### 实现思路

你当前是 TCP 长连接服务，所以建议把登录态和连接绑定。

有两种做法：

1. 先直接扩展 `ConnectionMeta`
2. 引入 `ConnectionSession` 或 `SessionManager`

如果想快一点，建议先做第一种：

- `authenticated`
- `user_id`
- `username`
- `token`

后面如果聊天功能多起来，再把这部分抽到 `SessionManager`。

### 这一阶段怎么测试

1. 登录成功后，在服务端打印当前连接对应用户信息

2. 增加一个临时需要登录的测试消息，例如：

- `whoami`

请求：

```json
{"msg_type":"whoami","seq":3,"token":"","data":{}}
```

预期：

- 未登录连接调用时返回未登录
- 已登录连接调用时返回当前 `user_id/username`

### 完成标志

- 服务端能识别当前连接是否已登录
- 同一个连接上的后续请求可以复用登录态

---

## 第九阶段：补充 TCP 分包与稳定性处理

### 目标

避免 JSON 协议在真实 TCP 场景下受粘包、半包影响。

### 实现思路

当前可以先采用：

- 一条 JSON 一行
- 以 `\n` 作为消息边界

然后在 `PacketCodec` 中处理：

- 多条消息一起到达
- 一条消息分多次到达
- 空消息

等第一版稳定后，再升级成：

- 长度前缀 + JSON

涉及文件：

- [include/codec/packet_codec.h](/home/lzq/coding/linux_server/include/codec/packet_codec.h)
- [src/codec/packet_codec.cc](/home/lzq/coding/linux_server/src/codec/packet_codec.cc)

### 这一阶段怎么测试

1. 一次发送两条 JSON

2. 手动拆开发送一条 JSON 的前半段和后半段

3. 验证服务端不会把两条消息拼错

4. 验证服务端不会把半包当完整包处理

### 完成标志

- 能正确处理多包和半包
- JSON 协议在 TCP 下稳定可用

---

## 第十阶段：联调与回归测试

### 目标

在完整链路上验证注册、登录、心跳、异常输入都正常。

### 实现思路

这一阶段主要做联调，不再新增太多设计。

建议重点覆盖：

- 注册成功
- 注册重复用户名
- 登录成功
- 登录密码错误
- 非法 JSON
- 缺少字段
- 未知消息类型
- 心跳

### 这一阶段怎么测试

建议整理一组固定命令反复回归：

```bash
printf '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}\n' | nc 127.0.0.1 8080
```

```bash
printf '{"msg_type":"register","seq":2,"token":"","data":{"username":"alice","password":"123456","nickname":"Alice"}}\n' | nc 127.0.0.1 8080
```

```bash
printf '{"msg_type":"login","seq":3,"token":"","data":{"username":"alice","password":"123456"}}\n' | nc 127.0.0.1 8080
```

```bash
printf '{"msg_type":"login","seq":4,"token":"","data":{"username":"alice","password":"wrong"}}\n' | nc 127.0.0.1 8080
```

```bash
printf '{"msg_type":"unknown","seq":5,"token":"","data":{}}\n' | nc 127.0.0.1 8080
```

### 完成标志

- 核心认证链路已打通
- 常见异常输入有稳定表现
- 工程已经具备继续扩展聊天功能的基础

---

## 推荐实现顺序

建议你按下面顺序推进，不容易乱：

1. 工程结构与构建稳定
2. JSON 编解码
3. MessageHandler 路由
4. MySQL 用户表
5. UserRepository
6. 注册流程
7. 登录流程
8. 连接登录态
9. TCP 分包
10. 联调回归

---

## 额外建议

### 先不要急着做太重的部分

第一版先别急着上：

- Redis
- 分布式会话
- 复杂线程池
- 多端登录策略
- 好友和群聊

先把最小闭环做稳：

- 注册
- 登录
- 查 MySQL
- 返回统一 JSON

### 遇到问题时优先排查的顺序

1. JSON 协议格式是否一致
2. handler 有没有正确分发
3. service 有没有正确调用 repository
4. repository 的 SQL 是否正确
5. MySQL 表结构是否和代码一致
6. TCP 分包是否把消息截断了

---

## 最终目标

当你完成上面这些阶段后，项目就会具备一个比较扎实的基础：

- 基于 TCP 的 JSON 协议
- 注册接口
- 登录接口
- 登录前查 MySQL 用户信息
- 可继续扩展到聊天服务器的模块结构

后续再往上加：

- 单聊
- 群聊
- 离线消息
- 在线状态
- 好友关系

就会顺很多。
