# =============================================================================
#  OnlineJudge Backend — multi-stage Dockerfile
#  依据 SPEC §3.2 / §6.4：仅依赖 Docker / Docker Compose，不在宿主机装任何 C++ 库
#
#  Build :  docker build -t oj_backend:1.0 ./backend
#  Run   :  docker run --rm -p 8080:8080 \
#                -v /var/run/docker.sock:/var/run/docker.sock \
#                oj_backend:1.0
# =============================================================================

# ---------- Stage 1 : builder -----------------------------------------------
FROM debian:bookworm-slim AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        git \
        ca-certificates \
        default-libmysqlclient-dev \
        libcurl4-openssl-dev \
        libargon2-dev \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# 1) 先拷贝构建脚本，最大化命中 Docker 缓存
COPY CMakeLists.txt    ./CMakeLists.txt
COPY cmake/            ./cmake/

# 2) 再拷贝源码（实现 Phase 1 期间 src/include 会被频繁改动）
COPY include/          ./include/
COPY src/              ./src/
COPY config/           ./config/

# 3) 配置 + 构建
#    - 关闭 FetchContent 的 UPDATE，保证离线 / 内网可复现
#    - 安装到 /install 方便拷贝
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOJ_BUILD_TESTS=OFF \
        -DFETCHCONTENT_FULLY_DISCONNECTED=OFF \
 && cmake --build build --parallel "$(nproc)" \
 && cmake --install build --prefix /install

# ---------- Stage 2 : runtime -----------------------------------------------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        default-mysql-client \
        libcurl4 \
        libargon2-1 \
        libmysqlclient21 \
        libssl3 \
        tini \
    && rm -rf /var/lib/apt/lists/*

# 非 root 运行（SPEC §6.4 判题机内部用 judge 用户；后端容器同样收敛权限）
RUN groupadd --system --gid 1000 oj \
    && useradd  --system --uid 1000 --gid oj --create-home --shell /usr/sbin/nologin oj

WORKDIR /app

# 拷贝编译产物
COPY --from=builder /install/bin/oj_backend   /usr/local/bin/oj_backend
COPY --from=builder /install/config/default.json  /app/config/default.json

# 日志目录（spdlog 轮转目录，参见 SPEC §3.2.3）
RUN mkdir -p /var/log/oj && chown -R oj:oj /var/log/oj /app
USER oj

EXPOSE 8080

# 健康检查：命中 Phase 1 的 GET /api/health
HEALTHCHECK --interval=15s --timeout=3s --start-period=20s --retries=5 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

# tini 负责收割僵尸进程，避免 judge worker 子进程泄漏
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["oj_backend", \
     "--config", "/app/config/default.json", \
     "--log-dir", "/var/log/oj"]
