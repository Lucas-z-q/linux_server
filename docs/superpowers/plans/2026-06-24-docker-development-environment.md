# Docker Development Environment Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide a reproducible local Docker Compose environment that builds and runs the chat server with healthy MySQL and Redis dependencies.

**Architecture:** A multi-stage Ubuntu image builds only the `server` target and runs it as an unprivileged user. A three-service Compose project supplies MySQL schema initialization, Redis persistence, private dependency networking, environment-based credentials, health checks, and a single published chat port.

**Tech Stack:** Docker Engine, Docker Compose, Ubuntu 22.04, CMake, C++17, MySQL 8, Redis 7

---

### Task 1: Define development configuration and Compose topology

**Files:**

- Create: `.env.example`
- Modify: `.gitignore`
- Modify: `.dockerignore`
- Create: `compose.yaml`
- Delete: `docker-compose.yml`
- Modify: `config/server.docker.json`

- [ ] **Step 1: Run configuration checks that expose the incomplete draft**

Run:

```bash
test -f .env.example
```

Expected: FAIL because `.env.example` does not exist.

Run:

```bash
grep -q '^\.env$' .gitignore
```

Expected: FAIL because the local environment file is not ignored.

- [ ] **Step 2: Add safe environment defaults**

Create `.env.example` with:

```dotenv
CHAT_SERVER_PORT=8080
MYSQL_DATABASE=chat
MYSQL_USER=chat_app
MYSQL_PASSWORD=chat_app_password
MYSQL_ROOT_PASSWORD=chat_root_password
REDIS_DATABASE=0
CHAT_SERVER_ID=server-1
```

Add this entry to `.gitignore`:

```gitignore
.env
```

Ensure `.dockerignore` excludes `.env`, `.env.*`, local build output, logs, Git metadata, editor configuration, agent configuration, and repository instruction files while retaining `.env.example`.

- [ ] **Step 3: Replace the draft Compose file**

Create `compose.yaml` with three services:

- `mysql` uses `mysql:8.0`, creates the application database and account from `.env`, mounts `sql/001_create_chat_tables.sql` read-only, persists `/var/lib/mysql`, and checks health with authenticated `mysqladmin ping`.
- `redis` uses `redis:7.0-alpine`, starts with `--appendonly yes`, persists `/data`, and checks health with `redis-cli ping`.
- `server` builds the repository Dockerfile, passes all supported MySQL and Redis environment overrides, waits for healthy dependencies, publishes only `${CHAT_SERVER_PORT:-8080}:8080`, and checks port `8080` with `nc`.

Use `unless-stopped` restart policies, omit obsolete top-level `version`, omit fixed container names, and do not publish MySQL or Redis ports.

Delete `docker-compose.yml` so Compose has one unambiguous default file.

- [ ] **Step 4: Remove credentials and file logging from Docker JSON**

Set `config/server.docker.json` to:

- listen on `0.0.0.0:8080`;
- use `mysql`, `chat_app`, an empty password, and database `chat` as non-sensitive defaults;
- enable Redis at host `redis`;
- keep existing pool, timeout, cache, session, deduplication, and rate-limit values;
- set `log.console` to `true` and `log.file_path` to an empty string.

- [ ] **Step 5: Validate Compose interpolation and topology**

Run:

```bash
docker compose --env-file .env.example config
```

Expected: PASS with services `mysql`, `redis`, and `server`; only the server service has a published port.

- [ ] **Step 6: Commit configuration**

```bash
git add .env.example .gitignore .dockerignore compose.yaml config/server.docker.json
git add -u docker-compose.yml
git commit -m "build: define Docker development services"
```

### Task 2: Build a minimal runtime server image

**Files:**

- Modify: `Dockerfile`

- [ ] **Step 1: Verify the draft image is not yet accepted**

Run:

```bash
grep -q 'COPY --from=builder.*/build/client' Dockerfile
```

Expected: PASS, demonstrating the draft still copies the unnecessary client binary and needs replacement.

- [ ] **Step 2: Implement the multi-stage image**

Update `Dockerfile` so the builder stage:

- uses `ubuntu:22.04`;
- installs `build-essential`, `cmake`, `ca-certificates`, `default-libmysqlclient-dev`, `libhiredis-dev`, `libcrypt-dev`, and `libgtest-dev`;
- configures a Release build;
- builds only the `server` target.

Update the runtime stage so it:

- uses `ubuntu:22.04`;
- installs `ca-certificates`, `libmariadb3`, `libhiredis0.14`, `libcrypt1`, and `netcat-openbsd`;
- creates a system user named `chat`;
- copies only `/build/server` and `config/server.docker.json`;
- runs from `/app` as `chat`;
- exposes port `8080`;
- executes `/app/server --config /app/config/server.docker.json`.

- [ ] **Step 3: Build the server image**

Run:

```bash
docker compose --env-file .env.example build server
```

Expected: PASS and produce the Compose server image.

- [ ] **Step 4: Inspect runtime identity and linked libraries**

Run:

```bash
docker compose --env-file .env.example run --rm --no-deps --entrypoint sh server -c 'test "$(id -u)" -ne 0 && ldd /app/server'
```

Expected: PASS, report a non-root user, and show no `not found` libraries.

- [ ] **Step 5: Commit the image**

```bash
git add Dockerfile
git commit -m "build: add chat server container image"
```

### Task 3: Document the Docker development workflow

**Files:**

- Modify: `README.md`
- Modify: `README_DOCKER.md`

- [ ] **Step 1: Identify invalid draft documentation**

Run:

```bash
grep -q 'file:///home/' README_DOCKER.md
```

Expected: PASS, demonstrating the draft contains machine-local links.

- [ ] **Step 2: Rewrite the Docker guide**

Update `README_DOCKER.md` to use repository-relative links and document:

- `cp .env.example .env`;
- `docker compose up --build -d`;
- `docker compose ps`;
- `docker compose logs -f server`;
- heartbeat verification with `printf` and `nc`;
- MySQL access through `docker compose exec mysql`;
- Redis access through `docker compose exec redis`;
- rebuild after C++ changes;
- `docker compose down`;
- `docker compose down -v` and the fact that schema initialization only runs on an empty MySQL volume.

State explicitly that only the chat port is published by default and that the sample passwords are for local development.

- [ ] **Step 3: Link the guide from the main README**

Add a concise Docker development section to `README.md` with the copy, start, status, and stop commands, then link to `README_DOCKER.md` for details.

- [ ] **Step 4: Validate Markdown**

Run:

```bash
git diff --check -- README.md README_DOCKER.md
```

Expected: PASS.

If a compatible Markdown linter is available, run:

```bash
mise run check-md
```

Expected: PASS. If `mise` is unavailable, record that tool limitation and retain the `git diff --check` result.

- [ ] **Step 5: Commit documentation**

```bash
git add README.md README_DOCKER.md
git commit -m "docs: add Docker development workflow"
```

### Task 4: Verify the complete environment

**Files:**

- No source changes expected

- [ ] **Step 1: Validate final Compose configuration**

Run:

```bash
docker compose --env-file .env.example config --quiet
```

Expected: PASS with exit code `0`.

- [ ] **Step 2: Start a clean environment**

Run:

```bash
docker compose --env-file .env.example down -v --remove-orphans
docker compose --env-file .env.example up --build -d
```

Expected: PASS.

- [ ] **Step 3: Wait for service health**

Run:

```bash
timeout 120 sh -c 'until [ "$(docker compose --env-file .env.example ps --format json | grep -c healthy)" -ge 3 ]; do sleep 2; done'
docker compose --env-file .env.example ps
```

Expected: MySQL, Redis, and server are running and healthy.

- [ ] **Step 4: Verify schema initialization**

Run:

```bash
docker compose --env-file .env.example exec -T mysql sh -c 'MYSQL_PWD="$MYSQL_PASSWORD" mysql -u"$MYSQL_USER" -D"$MYSQL_DATABASE" -Nse "SHOW TABLES"'
```

Expected: output includes `users`, `friendships`, `conversations`, `groups`, `group_members`, `conversation_members`, and `messages`.

- [ ] **Step 5: Verify the heartbeat protocol**

Run:

```bash
response="$(printf '%s\n' '{"msg_type":"heartbeat","seq":1,"token":"","data":{}}' | nc -w 3 127.0.0.1 "${CHAT_SERVER_PORT:-8080}")"
printf '%s\n' "$response"
printf '%s' "$response" | grep -q '"msg_type":"heartbeat_resp"'
printf '%s' "$response" | grep -q '"seq":1'
printf '%s' "$response" | grep -q '"code":0'
```

Expected: all checks PASS.

- [ ] **Step 6: Run native regression verification**

Run:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Expected: build succeeds and all registered tests pass. Any failure in the pre-existing modified `tests/auth_integration_test.cc` must be reported separately and must not be overwritten.

- [ ] **Step 7: Stop containers without deleting data**

Run:

```bash
docker compose --env-file .env.example down
```

Expected: containers and network are removed; named volumes remain.

- [ ] **Step 8: Review final scope**

Run:

```bash
git status --short
git diff HEAD~3 -- . ':(exclude)tests/auth_integration_test.cc'
```

Expected: Docker implementation and documentation changes are present, while the unrelated test modification remains untouched.
