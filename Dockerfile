FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ca-certificates \
    default-libmysqlclient-dev \
    libcrypt-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

COPY . .

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target server --parallel "$(nproc)"

FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libcrypt1 \
    libmysqlclient21 \
    netcat-openbsd \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd --system chat && \
    useradd --system --gid chat --home-dir /app --shell /usr/sbin/nologin chat

WORKDIR /app

COPY --from=builder --chown=chat:chat /src/build/server /app/server
COPY --from=builder --chown=chat:chat /src/config/server.docker.json /app/config/server.docker.json

USER chat

EXPOSE 8080

ENTRYPOINT ["/app/server"]
CMD ["--config", "/app/config/server.docker.json"]
