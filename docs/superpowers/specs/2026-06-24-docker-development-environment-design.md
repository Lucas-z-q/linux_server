# Docker Development Environment Design

## Overview

Add a Docker Compose development environment for the chat server. One command will build the C++ server image and start the server together with MySQL and Redis.

This environment targets local development and functional testing. It does not attempt to provide production deployment, orchestration, TLS termination, monitoring, or backup facilities.

## Goals

- Start the server, MySQL, and Redis with `docker compose up --build`.
- Build the C++ server in a reproducible multi-stage image.
- Initialize the existing MySQL schema automatically on a new database volume.
- Wait for MySQL and Redis health checks before starting the server.
- Keep database credentials outside committed JSON configuration.
- Expose the chat server on host port `8080`.
- Persist MySQL and Redis data in named volumes.
- Support resetting the environment with `docker compose down -v`.
- Document common development, debugging, and cleanup commands.
- Verify the running server with a newline-delimited heartbeat request.

## Non-Goals

- Do not modify C++ application behavior.
- Do not provide a source-mounted hot-reload development container.
- Do not package MySQL, Redis, and the server into one container.
- Do not define a production deployment architecture.
- Do not add Kubernetes, reverse proxy, TLS, metrics, or backup configuration.
- Do not expose MySQL or Redis ports to the host by default.
- Do not commit real credentials, generated files, logs, or local tool state.

## Selected Approach

Use a standard three-service Docker Compose environment:

- `server`: the C++ chat server built from the repository Dockerfile.
- `mysql`: the official MySQL 8 image with schema initialization and a named data volume.
- `redis`: the official Redis 7 Alpine image with persistence and a named data volume.

The Dockerfile will use separate build and runtime stages. Compose will place all services on its default private network, where the server resolves dependencies by service name.

This approach preserves service boundaries and gives developers a reproducible environment without requiring local MySQL, Redis, or C++ build dependencies.

## Files

### Dockerfile

The Dockerfile will:

- Use a fixed Ubuntu base version for both stages.
- Install the compiler, CMake, MySQL client development package, hiredis development package, crypt development package, and required certificate tooling in the build stage.
- Copy the source and perform a Release CMake build.
- Copy only the `server` binary and Docker configuration into the runtime stage.
- Install only the dynamic libraries required by the server.
- Create an unprivileged runtime user and writable working directories.
- Expose TCP port `8080`.
- Start the server with the Docker-specific configuration file.

The simple client and test binaries will not be included in the runtime image because they are not required to run the service.

### compose.yaml

The Compose file will:

- Build the `server` service from the repository Dockerfile.
- Start MySQL with a dedicated application database and non-root application account.
- Mount `sql/001_create_chat_tables.sql` into the MySQL initialization directory as read-only.
- Start Redis with append-only persistence enabled.
- Define health checks for MySQL and Redis.
- Make `server` depend on healthy MySQL and Redis services.
- Define a TCP health check for the server.
- Publish only `${CHAT_SERVER_PORT:-8080}:8080`.
- Store MySQL and Redis state in named volumes.
- Use `unless-stopped` restart policies for local convenience.

The Compose file will not use fixed `container_name` values, allowing multiple project copies to run with distinct Compose project names.

### config/server.docker.json

The Docker configuration will:

- Listen on `0.0.0.0:8080`.
- Enable Redis.
- Contain only non-sensitive defaults.
- Log to the container console.
- Keep existing timeout, pool, cache, session, deduplication, and rate-limit defaults unless Docker operation requires a focused change.

MySQL and Redis hosts, ports, usernames, passwords, database names, and server identifiers will be supplied or overridden through environment variables.

### .env.example

The example environment file will document safe local defaults for:

- Host chat server port.
- MySQL database name.
- MySQL application username.
- MySQL application password.
- MySQL root password.
- Redis database number.
- Chat server instance identifier.

The committed file will contain development-only placeholder values. The actual `.env` file will remain ignored by Git.

### .dockerignore

The build context will exclude:

- Git metadata.
- Local build output.
- Logs.
- Editor and local agent configuration.
- Local environment files.
- Temporary and cache files.
- Repository instruction files that are not needed to build the application.

Source files, CMake configuration, third-party source dependencies, Docker configuration, and SQL initialization scripts will remain available to the image build.

### Documentation

Docker usage will be documented in the main README or a linked Docker guide. The documentation will cover:

- Creating `.env` from `.env.example`.
- Building and starting the environment.
- Checking service health.
- Viewing server, MySQL, and Redis logs.
- Sending a heartbeat request.
- Running MySQL and Redis diagnostic commands through `docker compose exec`.
- Stopping services while preserving data.
- Removing named volumes to reset the environment.
- Rebuilding after source changes.

Documentation links will use repository-relative paths.

## Configuration and Secret Handling

The committed Docker JSON file will not contain passwords. Compose will pass credentials through environment variables supported by the existing configuration loader.

MySQL will create the application database and application account from the official image environment variables. The server will connect with that application account instead of the MySQL root account.

The server configuration precedence remains:

1. Explicit `--config` argument.
2. `CHAT_CONFIG_PATH`.
3. Default local configuration path.

The container entrypoint will select `config/server.docker.json`, while Compose environment variables override dependency addresses and credentials.

## Startup and Data Flow

```text
docker compose up --build
  -> build server image
  -> start mysql and redis
  -> initialize a new mysql volume from sql/001_create_chat_tables.sql
  -> wait for mysql and redis health checks
  -> start server with Docker configuration and environment overrides
  -> publish host port 8080 to the server container
```

MySQL initialization scripts run only when its data directory is empty. Developers must use `docker compose down -v` when they intentionally need to recreate the database from the initialization script.

## Health Checks and Error Handling

MySQL health will be checked with `mysqladmin ping` using credentials available inside the container. Redis health will be checked with `redis-cli ping`.

The server health check will verify that its TCP listening port accepts connections. A successful TCP check proves that the process completed dependency initialization and entered its listening loop. Protocol correctness will be verified separately with the heartbeat smoke test.

If dependency initialization fails, the dependent server will not start. If server configuration or application initialization fails, its container will exit and the failure will remain visible in `docker compose ps` and `docker compose logs server`.

Health checks must not suppress startup errors or turn a failed application start into a healthy state.

## Security Boundaries

- The server runs as a non-root user.
- The server uses a non-root MySQL application account.
- MySQL and Redis are reachable only inside the Compose network by default.
- No real passwords are committed.
- SQL initialization mounts are read-only.
- Only the chat server TCP port is published to the host.

These controls are appropriate for local development, but the environment is not a production security baseline.

## Verification

Static validation:

```bash
docker compose config
docker compose build server
```

Runtime validation:

```bash
docker compose up -d
docker compose ps
docker compose logs server
```

All three services must be running and healthy. The server must answer this newline-delimited request on the published port:

```bash
printf '%s\n' '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}' |
  nc 127.0.0.1 8080
```

The response must be valid JSON, use the matching sequence number, report success, and identify the heartbeat response message type.

The implementation will also run the existing native build and relevant test suite when available, ensuring Docker support does not alter application behavior.

## Acceptance Criteria

- `docker compose config` succeeds without warnings caused by obsolete Compose syntax.
- `docker compose build server` produces the runtime image.
- `docker compose up -d` starts healthy MySQL, Redis, and server services.
- A fresh MySQL volume contains the tables from `sql/001_create_chat_tables.sql`.
- The server uses the application MySQL account and connects to dependencies by Compose service name.
- Only the configured chat server port is published to the host.
- The heartbeat smoke test returns a successful response.
- No credential is embedded in `config/server.docker.json`.
- Docker usage and reset behavior are documented.
- Existing unrelated working-tree changes remain untouched.
