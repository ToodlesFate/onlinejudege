# =============================================================================
#  judge-images/README.md —— 判题镜像构建说明
#
#  judge-tool 编译产物 + entrypoint.sh 共同构成本仓库的 5 个判题镜像：
#    judge-cpp:1.0     judge-c:1.0     judge-java:1.0
#    judge-python:1.0  judge-go:1.0
#
#  镜像内部布局（SPEC §3.4 / §6.1）：
#    /judge/bin/judge          ← 编译后的 C++ judge 工具
#    /judge/bin/entrypoint.sh  ← 通用入口
#    /tmp/oj/<id>/             ← 单次提交工作目录（卷载，run 结束清除）
#
#  构建步骤（示意）：
#    1) 在 judge-tool/build/ 跑 ninja  → 产出 ./judge
#    2) cp ./judge <image-root>/judge/bin/
#    3) cp common/entrypoint.sh <image-root>/judge/bin/
#    4) 写每种语言的 Dockerfile（基础镜像 + judge + entrypoint）
#
#  后续阶段（Phase 4 余下任务）会：
#    - 写 5 个 judge 镜像的 Dockerfile
#    - 把 entrypoint.sh 拷到各镜像 /judge/bin/
#    - docker-compose build 一键构建
# =============================================================================

# 编译（开发机）
cd judge-tool
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ls -la build/judge

# 部署到镜像
mkdir -p ../judge-images/cpp/judge/bin
cp build/judge ../judge-images/cpp/judge/bin/judge
cp common/entrypoint.sh ../judge-images/cpp/judge/bin/entrypoint.sh
chmod +x ../judge-images/cpp/judge/bin/entrypoint.sh

# 对 c/java/python/go 镜像重复
for lang in c java python go; do
    mkdir -p ../judge-images/$lang/judge/bin
    cp build/judge ../judge-images/$lang/judge/bin/judge
    cp common/entrypoint.sh ../judge-images/$lang/judge/bin/entrypoint.sh
    chmod +x ../judge-images/$lang/judge/bin/entrypoint.sh
done
