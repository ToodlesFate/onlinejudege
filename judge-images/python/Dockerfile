# =============================================================================
#  judge-images/python/Dockerfile —— judge-python:1.0
#  SPEC §6.3：基于 python:3.12-slim；含 python3 + judge
# =============================================================================
FROM python:3.12-slim

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

LABEL org.opencontainers.image.title="judge-python" \
      org.opencontainers.image.version="1.0" \
      oj.language="python"

HEALTHCHECK --interval=30s --timeout=3s --start-period=2s --retries=2 \
    CMD /judge/bin/judge --help || exit 1
