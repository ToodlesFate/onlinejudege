# =============================================================================
#  judge-images/cpp/Dockerfile —— judge-cpp:1.0
#  SPEC §6.3：基于 gcc:13-bookworm；含 g++ + 已编译 judge 工具
#  用 multi-stage 瘦身：先在 build 镜像里编译 judge，最终镜像只装 g++ runtime
# =============================================================================

# ---- Stage 1: build judge binary (宿主机已经构建好 —— 这里只 COPY 进来) ----
# 不再二次构建 judge；直接用构建脚本产物

# ---- Stage 2: 最终镜像 ----
FROM debian:bookworm-slim

ARG JUDGE_UID=1000
ARG DEBIAN_FRONTEND=noninteractive

# 仅装 g++（含 libstdc++/libgcc），不装 build-essentials 全套
# 同时装 diff / coreutils / sh（base 已有但显式列出）
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        g++ \
        libc6-dev \
        gcc \
 && rm -rf /var/lib/apt/lists/*

# 复制预先构建好的 judge 二进制与入口脚本
COPY --chown=${JUDGE_UID}:${JUDGE_UID} build/judge    /judge/bin/judge
COPY --chown=${JUDGE_UID}:${JUDGE_UID} common/entrypoint.sh /judge/bin/entrypoint.sh
RUN chmod 0755 /judge/bin/judge /judge/bin/entrypoint.sh

# SPEC §3.4：非 root 用户 UID 1000
RUN groupadd -g ${JUDGE_UID} judge \
 && useradd  -u ${JUDGE_UID} -g judge -d /judge -s /bin/sh -M judge

# 工作目录
WORKDIR /judge/work
RUN chown -R judge:judge /judge

USER judge

# 入口（SPEC §6.1）
ENTRYPOINT ["/judge/bin/entrypoint.sh"]

# 默认元数据
LABEL org.opencontainers.image.title="judge-cpp" \
      org.opencontainers.image.version="1.0" \
      oj.language="cpp"

# 健康检查 —— judge --help 退出 0 即 OK
HEALTHCHECK --interval=30s --timeout=3s --start-period=2s --retries=2 \
    CMD /judge/bin/judge --help || exit 1
