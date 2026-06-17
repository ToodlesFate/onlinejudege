# =============================================================================
#  judge-images/build.sh —— 一键构建 5 个 judge 镜像
#  依据 SPEC §6.3
#
#  流程：
#    1) 在 judge-tool/build/ 跑 ninja，确认 ./judge 存在
#    2) 遍历 5 种语言，docker build -t judge-<lang>:1.0
#    3) 显示镜像大小
#
#  用法：bash judge-images/build.sh
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
JUDGE_TOOL_DIR="$ROOT_DIR/judge-tool"
JUDGE_BIN="$JUDGE_TOOL_DIR/build/judge"

LANGS=(cpp c java python go)

# 1) 构建 judge 工具 —— 在容器内（gcc:13-bookworm）做，glibc 2.36 匹配最终镜像
#    容器以当前 host 用户运行，避免 root 写出的文件宿主无法删除
echo "==> 构建 judge 工具（容器内，glibc 2.36 兼容）"
# 用临时容器编译到容器内 /build，再用 docker cp 把二进制拿出来；
# 避免容器内 root 写的文件污染宿主 judge-tool 目录
# 用 debian 自带 libnlohmann-json3-dev 替代 FetchContent，避免容器内 git fetch 超时
BUILD_TMP=$(mktemp -d /tmp/judge-build-XXXXXX)
CONTAINER_NAME="judge-build-$RANDOM"
docker run --name "$CONTAINER_NAME" \
    -v "$JUDGE_TOOL_DIR:/src:ro" \
    debian:bookworm-slim \
    bash -c "
        set -e
        apt-get update -qq
        DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
            g++ cmake ninja-build nlohmann-json3-dev
        mkdir -p /build && cd /build
        cmake -S /src -B /build -DJUDGE_STATIC=ON -DCMAKE_BUILD_TYPE=Release \
            -DJUDGE_NO_FETCHCONTENT=ON -DJUDGE_TOOL_BUILD_TESTS=OFF -G Ninja
        cmake --build /build --target judge -j 4
        ls -la /build/judge
    " 2>&1 | tail -5
docker cp "$CONTAINER_NAME:/build/judge" "$BUILD_TMP/judge"
docker rm "$CONTAINER_NAME" >/dev/null
JUDGE_BIN="$BUILD_TMP/judge"
chmod +x "$JUDGE_BIN"

if [[ ! -x "$JUDGE_BIN" ]]; then
    echo "ERROR: judge binary not found at $JUDGE_BIN" >&2
    exit 1
fi
echo "    judge: $(ls -lh "$JUDGE_BIN" | awk '{print $5}')"

# 1.5) 把 judge 二进制复制到 judge-images/build/judge —— Docker build context 要求
mkdir -p "$SCRIPT_DIR/build"
cp -f "$JUDGE_BIN" "$SCRIPT_DIR/build/judge"

# 2) 构建 5 个镜像
for lang in "${LANGS[@]}"; do
    echo "==> docker build -t judge-${lang}:1.0  ($lang)"
    docker build \
        --build-arg JUDGE_UID=1000 \
        -t "judge-${lang}:1.0" \
        -f "$SCRIPT_DIR/${lang}/Dockerfile" \
        "$SCRIPT_DIR"
done

# 3) 镜像大小
echo
echo "==> 镜像大小"
docker images --format "  {{.Repository}}:{{.Tag}}\t{{.Size}}" | grep '^judge-'
