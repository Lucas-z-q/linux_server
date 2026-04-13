# Linux Chat Server Protocol

## 1. Overview

本项目采用 `TCP` 长连接进行通信，应用层数据使用 `JSON` 格式传输。

当前协议主要用于实现以下基础能力：

- 用户注册
- 用户登录
- 用户登出
- 心跳保活

后续可在此协议基础上继续扩展：

- 单聊消息
- 群聊消息
- 好友系统
- 离线消息
- 多端登录

---

## 2. Protocol Rules

### 2.1 Transport Layer

- 传输协议：`TCP`
- 通信方式：长连接
- 字符编码：`UTF-8`

### 2.2 Message Format

每条业务消息使用一段完整的 `JSON` 表示。

第一版协议采用：

- 每条 JSON 消息结尾追加换行符 `\n`
- 服务端按行读取并解析消息

示例：

```text
{"msg_type":"login","seq":2,"token":"","data":{"username":"alice","password":"123456"}}\n
```

### 2.3 General Message Structure

客户端请求格式：

```json
{
  "msg_type": "login",
  "seq": 1001,
  "token": "",
  "data": {}
}
```

服务端响应格式：

```json
{
  "msg_type": "login_resp",
  "seq": 1001,
  "code": 0,
  "message": "ok",
  "data": {}
}
```

字段说明：

| 字段名 | 类型 | 说明 |
|---|---|---|
| `msg_type` | string | 消息类型 |
| `seq` | int | 请求序号，由客户端生成，服务端响应原样返回 |
| `token` | string | 认证令牌，未登录时可为空 |
| `data` | object | 业务数据 |
| `code` | int | 响应状态码，`0` 表示成功，仅响应包含 |
| `message` | string | 响应描述信息，仅响应包含 |

---

## 3. Message Types

当前版本支持以下消息类型：

- `register`
- `login`
- `logout`
- `heartbeat`

后续预留消息类型：

- `send_message`
- `pull_offline_messages`
- `friend_list`
- `create_group`

---

## 4. Register

### 4.1 Request

```json
{
  "msg_type": "register",
  "seq": 1,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456",
    "nickname": "Alice"
  }
}
```

### 4.2 Request Fields

| 字段名 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `username` | string | 是 | 用户名，要求唯一 |
| `password` | string | 是 | 用户密码，服务端只保存哈希值 |
| `nickname` | string | 否 | 用户昵称 |

### 4.3 Success Response

```json
{
  "msg_type": "register_resp",
  "seq": 1,
  "code": 0,
  "message": "register success",
  "data": {
    "user_id": 10001
  }
}
```

### 4.4 Failure Response

```json
{
  "msg_type": "register_resp",
  "seq": 1,
  "code": 1002,
  "message": "username already exists",
  "data": {}
}
```

---

## 5. Login

### 5.1 Request

```json
{
  "msg_type": "login",
  "seq": 2,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456"
  }
}
```

### 5.2 Request Fields

| 字段名 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `username` | string | 是 | 用户名 |
| `password` | string | 是 | 明文密码，服务端收到后进行校验 |

### 5.3 Success Response

```json
{
  "msg_type": "login_resp",
  "seq": 2,
  "code": 0,
  "message": "login success",
  "data": {
    "user_id": 10001,
    "nickname": "Alice",
    "token": "token_xxx"
  }
}
```

### 5.4 Failure Response

```json
{
  "msg_type": "login_resp",
  "seq": 2,
  "code": 1004,
  "message": "invalid username or password",
  "data": {}
}
```

### 5.5 Server Processing Rules

登录请求的服务端处理流程：

1. 解析 `username` 和 `password`
2. 查询 MySQL 用户表
3. 判断用户是否存在
4. 取出数据库中保存的密码哈希
5. 对输入密码按同样规则计算哈希
6. 比对哈希结果
7. 登录成功后建立登录态，并返回 `token`

说明：

- 登录前必须查询 MySQL 用户信息
- 登录失败时建议统一返回 `invalid username or password`
- 不建议区分“用户不存在”和“密码错误”的提示内容

---

## 6. Logout

### 6.1 Request

```json
{
  "msg_type": "logout",
  "seq": 3,
  "token": "token_xxx",
  "data": {}
}
```

### 6.2 Success Response

```json
{
  "msg_type": "logout_resp",
  "seq": 3,
  "code": 0,
  "message": "logout success",
  "data": {}
}
```

---

## 7. Heartbeat

### 7.1 Request

```json
{
  "msg_type": "heartbeat",
  "seq": 4,
  "token": "token_xxx",
  "data": {}
}
```

### 7.2 Success Response

```json
{
  "msg_type": "heartbeat_resp",
  "seq": 4,
  "code": 0,
  "message": "pong",
  "data": {
    "server_time": 1710000000
  }
}
```

### 7.3 Purpose

心跳用于：

- 保持 TCP 长连接活跃
- 检测连接是否断开
- 更新服务端最近活跃时间

---

## 8. Error Codes

| 错误码 | 含义 |
|---|---|
| `0` | 成功 |
| `1001` | 参数错误 |
| `1002` | 用户名已存在 |
| `1003` | 用户不存在 |
| `1004` | 用户名或密码错误 |
| `1005` | 未登录 |
| `1006` | token 无效 |
| `1007` | 权限不足 |
| `2001` | 数据库错误 |
| `2002` | 服务器内部错误 |

建议错误码分层：

- `1xxx`：认证与用户相关错误
- `2xxx`：系统与数据库相关错误
- `3xxx`：聊天业务相关错误

---

## 9. Authentication Strategy

当前协议支持两种认证思路：

### 9.1 Connection-Based Authentication

服务端在连接上下文中记录当前连接的登录状态，例如：

- 是否已登录
- 当前用户 ID
- 当前用户名

适用于当前项目的第一版实现，简单直接。

### 9.2 Token-Based Authentication

登录成功后，服务端生成 `token` 并返回给客户端。后续请求携带 `token` 完成认证。

适用于后续扩展场景：

- 断线重连
- 多端登录
- 分布式部署

### 9.3 Recommendation

建议第一版：

- 协议中保留 `token` 字段
- 服务端主要使用“连接态认证”
- 后续逐步过渡到“连接态 + token”结合

---

## 10. Database Notes

登录和注册功能依赖 MySQL 用户表。

建议用户表至少包含以下字段：

| 字段名 | 说明 |
|---|---|
| `id` | 用户 ID |
| `username` | 用户名，唯一索引 |
| `password_hash` | 密码哈希 |
| `nickname` | 昵称 |
| `status` | 用户状态 |
| `created_at` | 创建时间 |
| `updated_at` | 更新时间 |

说明：

- 不保存明文密码
- 注册时先查重再插入
- 登录时先查 MySQL 用户信息再进行密码校验
- SQL 操作建议使用预处理语句，避免 SQL 注入

---

## 11. First Version Implementation Plan

建议第一版按以下顺序实现：

1. 定义 JSON 协议
2. 约定使用 `\n` 作为消息边界
3. 实现 `register`
4. 实现 `login`
5. 实现 `logout`
6. 实现 `heartbeat`
7. 登录成功后维护连接登录态
8. 后续业务请求统一先做认证检查

---

## 12. Future Improvements

当系统并发量上来后，建议逐步升级：

- 将 `JSON + \n` 改为“长度前缀 + JSON”
- 增加 MySQL 连接池
- 将数据库访问从 I/O 线程中分离
- 引入业务线程池
- 使用 `SessionManager` 统一管理在线用户
- 支持离线消息和多端登录

---

## 13. Example

注册请求：

```json
{
  "msg_type": "register",
  "seq": 1,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456",
    "nickname": "Alice"
  }
}
```

登录请求：

```json
{
  "msg_type": "login",
  "seq": 2,
  "token": "",
  "data": {
    "username": "alice",
    "password": "123456"
  }
}
```

登录响应：

```json
{
  "msg_type": "login_resp",
  "seq": 2,
  "code": 0,
  "message": "login success",
  "data": {
    "user_id": 10001,
    "nickname": "Alice",
    "token": "token_xxx"
  }
}
```
