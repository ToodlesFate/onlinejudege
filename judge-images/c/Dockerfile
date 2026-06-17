# =============================================================================
#  judge-images/c/Dockerfile —— judge-c:1.0
#  SPEC §6.3：基于 debian-slim + gcc
# =============================================================================
FROM debian:bookworm-slim

ARG JUDGE_UID=1000
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        gcc \
        libc6-dev \
 && rm -rf /var/lib/apt/lists/*

COPY --chown=${JUDGE_UID}:${JUDGE_UID} build/judge    /judge/bin/judge
COPY --chown=${JUDGE_UID}:${JUDGE_UID} common/entrypoint.sh /judge/bin/entrypoint.sh
RUN chmod 0755 /judge/bin/judge /judge/bin/entrypoint.sh

RUN groupadd -g ${JUDGE_UID} judge \
 && useradd  -u ${JUDGE_UID} -g judge -d /judge -s /bin/sh -M judge

WORKDIR /judge/work
RUN chown -R judge:judge /judge

USER judge

ENTRYPOINT ["/judge/bin/entrypoint.sh"]

LABEL org.opencontainers.image.title="judge-c" \
      org.opencontainers.image.version="1.0" \
      oj.language="c"

HEALTHCHECK --interval=30s --timeout=3s --start-period=2s --retries=2 \
    CMD /judge/bin/judge --help || exit 1
