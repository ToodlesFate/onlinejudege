# =============================================================================
#  judge-images/go/Dockerfile —— judge-go:1.0
#  SPEC §6.3：基于 debian-slim + Go
#  说明：原计划 golang:1.22-alpine，但 Alpine musl + gcompat 与 glibc 2.36 符号
#  不完全兼容（缺 __isoc23_* 等）；改用 debian-slim 直接装 Go，glibc 一致
# =============================================================================
FROM debian:bookworm-slim

ARG JUDGE_UID=1000
ARG DEBIAN_FRONTEND=noninteractive

# Go 1.22 在 bookworm 仓库里没有；用 golang-go backports 或下载官方
# 简单做法：拉官方 Go 1.22 linux-amd64
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        wget \
        ca-certificates \
 && wget -q https://go.dev/dl/go1.22.10.linux-amd64.tar.gz -O /tmp/go.tar.gz \
 && tar -C /usr/local -xzf /tmp/go.tar.gz \
 && rm /tmp/go.tar.gz \
 && rm -rf /var/lib/apt/lists/*

ENV GOROOT=/usr/local/go
ENV PATH=$PATH:/usr/local/go/bin

ARG JUDGE_UID=1000

COPY --chown=${JUDGE_UID}:${JUDGE_UID} build/judge    /judge/bin/judge
COPY --chown=${JUDGE_UID}:${JUDGE_UID} common/entrypoint.sh /judge/bin/entrypoint.sh
RUN chmod 0755 /judge/bin/judge /judge/bin/entrypoint.sh

RUN groupadd -g ${JUDGE_UID} judge \
 && useradd  -u ${JUDGE_UID} -g judge -d /judge -s /bin/sh -M judge

WORKDIR /judge/work
RUN chown -R judge:judge /judge

USER judge

ENTRYPOINT ["/judge/bin/entrypoint.sh"]

LABEL org.opencontainers.image.title="judge-go" \
      org.opencontainers.image.version="1.0" \
      oj.language="go"

HEALTHCHECK --interval=30s --timeout=3s --start-period=2s --retries=2 \
    CMD /judge/bin/judge --help || exit 1
