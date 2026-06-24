# Docker 开发环境

项目提供基于 Docker Compose 的本地开发环境，包括聊天服务器、MySQL 和 Redis。宿主机只需要安装 Docker Engine、Docker Compose，以及用于发送心跳请求的 `nc`。

## 组成

- [Dockerfile](Dockerfile)：使用 Ubuntu 24.04 多阶段构建，只将 `server` 和运行依赖放入最终镜像，并以非 root 用户运行。
- [compose.yaml](compose.yaml)：编排 `server`、`mysql` 和 `redis`，配置依赖健康检查与数据卷。
- [config/server.docker.json](config/server.docker.json)：容器内的非敏感默认配置。
- [.env.example](.env.example)：本地开发环境变量示例。
- [sql/001_create_chat_tables.sql](sql/001_create_chat_tables.sql)：首次创建 MySQL 数据卷时执行的表结构脚本。

MySQL 和 Redis 默认只加入 Compose 私有网络，不向宿主机发布端口。聊天服务器默认发布到 `127.0.0.1` 可访问的宿主机端口 `8080`。

## 启动

先创建本地环境文件：

```bash
cp .env.example .env
```

`.env.example` 中的密码只用于本地开发。需要时可以修改 `.env`，该文件已被 Git 忽略。

构建镜像并在后台启动全部服务：

```bash
docker compose up --build -d
```

查看容器及健康状态：

```bash
docker compose ps
```

正常情况下，`mysql`、`redis` 和 `server` 都应显示为 `healthy`。

## 查看日志

查看服务器日志：

```bash
docker compose logs -f server
```

分别查看 MySQL 和 Redis 日志：

```bash
docker compose logs -f mysql
docker compose logs -f redis
```

服务端日志写入容器标准输出，不依赖宿主机日志目录权限。

## 心跳验证

服务启动后发送一条换行分隔的 JSON 心跳请求：

```bash
printf '%s\n' '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}' |
  nc -w 3 127.0.0.1 8080
```

响应应包含以下字段：

```json
{
  "code": 0,
  "msg_type": "heartbeat_resp",
  "seq": 1
}
```

实际响应还会包含 `data` 和 `message` 字段。

如果在 `.env` 中修改了 `CHAT_SERVER_PORT`，心跳命令也要使用对应的宿主机端口。

## 依赖调试

进入 MySQL 客户端：

```bash
docker compose exec mysql sh -c \
  'MYSQL_PWD="$MYSQL_PASSWORD" mysql -u"$MYSQL_USER" "$MYSQL_DATABASE"'
```

查看已创建的数据表：

```sql
SHOW TABLES;
```

进入 Redis 客户端：

```bash
docker compose exec redis redis-cli
```

检查 Redis：

```text
PING
```

## 修改代码后重建

C++ 源码被复制到镜像构建上下文，不会实时挂载。修改源码后重新构建并替换服务器容器：

```bash
docker compose up --build -d server
```

随后使用以下命令确认状态和日志：

```bash
docker compose ps
docker compose logs --tail=100 server
```

## 停止与重置

停止并删除容器和 Compose 网络，但保留 MySQL 与 Redis 数据卷：

```bash
docker compose down
```

删除容器、网络和数据卷，完全重置开发环境：

```bash
docker compose down -v
```

MySQL 的 `/docker-entrypoint-initdb.d` 初始化脚本只会在数据目录为空时执行。修改 SQL 初始化脚本后，需要执行 `docker compose down -v`，再重新启动环境。

## 常用配置

可在 `.env` 中修改：

| 变量 | 默认值 | 作用 |
| --- | --- | --- |
| `CHAT_SERVER_PORT` | `8080` | 发布到宿主机的聊天服务端口 |
| `MYSQL_DATABASE` | `chat` | 应用数据库名 |
| `MYSQL_USER` | `chat_app` | 应用数据库账号 |
| `MYSQL_PASSWORD` | `chat_app_password` | 应用数据库密码 |
| `MYSQL_ROOT_PASSWORD` | `chat_root_password` | 本地 MySQL root 密码 |
| `REDIS_DATABASE` | `0` | Redis 数据库编号 |
| `CHAT_SERVER_ID` | `server-1` | 当前聊天服务实例 ID |
