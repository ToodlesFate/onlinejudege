# =============================================================================
#  judge-images/java/Dockerfile —— judge-java:1.0
#  SPEC §6.3：基于 eclipse-temurin:21-jdk-jammy；含 javac + java + judge
#  SPEC §6.2：Java 运行命令 = `java -Xss64m -Xmx<题目内存>M Main`
# 镜像已自带 JRE/JDK 21；无需额外安装
# =============================================================================
FROM eclipse-temurin:21-jdk-jammy

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

LABEL org.opencontainers.image.title="judge-java" \
      org.opencontainers.image.version="1.0" \
      oj.language="java"

HEALTHCHECK --interval=30s --timeout=3s --start-period=2s --retries=2 \
    CMD /judge/bin/judge --help || exit 1
