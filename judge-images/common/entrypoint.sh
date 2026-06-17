#!/bin/bash
# =============================================================================
#  judge-images/common/entrypoint.sh
#  SPEC §6.1 容器内 entrypoint —— 每个 judge 镜像（cpp/c/java/python/go）通用
#
#  DockerClient 在主机端准备：
#    /tmp/oj/<submission_id>/
#    ├── src/                  ← 源代码（main.cpp / Main.java / main.py / main.go）
#    ├── testcases/            ← 1.in, 1.out, 2.in, 2.out, ...
#    ├── meta.json             ← {language, time_limit_ms, memory_limit_mb, output_limit_mb}
#    └── result/               ← 输出目录（卷载）
#
#  流程：
#    1) chdir 到工作目录
#    2) 执行 judge 二进制 —— 内部会
#       a) 读 meta.json
#       b) 编译（如有）→ 失败时写 compile.log + summary.json → 退出 10
#       c) 对每个 testcases/N.in 跑编译产物 → 写 per_case.json + summary.json
#       d) 退出 0
# =============================================================================

set -e
cd /judge/work

# judge 二进制在镜像内固定为 /judge/bin/judge
/judge/bin/judge \
    --meta   /judge/work/meta.json \
    --src    /judge/work/src \
    --tests  /judge/work/testcases \
    --out    /judge/work/result
