#!/bin/bash
# =============================================================================
#  judge-images/smoke.sh —— 5 个镜像的 smoke test
#  对每种语言跑一个 AC 测试点，验证容器内 judge 工具 + entrypoint 能工作
#
#  流程：
#    1) 准备 /tmp/oj_smoke/<id>/{src,testcases,result,meta.json}
#    2) docker run -v 挂载该目录
#    3) 检查 result/summary.json 看 result 字段
#    4) 清理
# =============================================================================

set -uo pipefail

IMAGES=("judge-cpp:1.0" "judge-c:1.0" "judge-java:1.0" "judge-python:1.0" "judge-go:1.0")

WORK=/tmp/oj_smoke
rm -rf $WORK
mkdir -p $WORK

pass=0
fail=0

# ---------- AC 用例：每种语言一个 hello world 风格 ----------
write_case_cpp() {
    cat > $WORK/src/main.cpp <<'EOF'
#include <cstdio>
int main() {
    int a, b; scanf("%d%d", &a, &b);
    printf("%d\n", a + b);
    return 0;
}
EOF
    cat > $WORK/meta.json <<EOF
{"language":"cpp","time_limit_ms":2000,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "2 3" > $WORK/testcases/1.in
    echo "5"   > $WORK/testcases/1.out
}
write_case_c() {
    cat > $WORK/src/main.c <<'EOF'
#include <stdio.h>
int main(void) {
    int a, b; scanf("%d%d", &a, &b);
    printf("%d\n", a + b);
    return 0;
}
EOF
    cat > $WORK/meta.json <<EOF
{"language":"c","time_limit_ms":2000,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "2 3" > $WORK/testcases/1.in
    echo "5"   > $WORK/testcases/1.out
}
write_case_java() {
    mkdir -p $WORK/src
    cat > $WORK/src/Main.java <<'EOF'
import java.util.Scanner;
public class Main {
    public static void main(String[] args) {
        Scanner sc = new Scanner(System.in);
        int a = sc.nextInt(), b = sc.nextInt();
        System.out.println(a + b);
    }
}
EOF
    cat > $WORK/meta.json <<EOF
{"language":"java","time_limit_ms":5000,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "2 3" > $WORK/testcases/1.in
    echo "5"   > $WORK/testcases/1.out
}
write_case_python() {
    cat > $WORK/src/main.py <<'EOF'
import sys
for line in sys.stdin:
    a, b = map(int, line.split())
    print(a + b)
EOF
    cat > $WORK/meta.json <<EOF
{"language":"python","time_limit_ms":5000,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "2 3" > $WORK/testcases/1.in
    echo "5"   > $WORK/testcases/1.out
}
write_case_go() {
    cat > $WORK/src/main.go <<'EOF'
package main
import "fmt"
func main() {
    var a, b int
    fmt.Scanf("%d %d", &a, &b)
    fmt.Printf("%d\n", a + b)
}
EOF
    cat > $WORK/meta.json <<EOF
{"language":"go","time_limit_ms":5000,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "2 3" > $WORK/testcases/1.in
    echo "5"   > $WORK/testcases/1.out
}

# ---------- 跑一个 image，期望 result == AC ----------
run_ac() {
    local img="$1"
    local lang="$2"
    rm -rf $WORK/* && mkdir -p $WORK/src $WORK/testcases $WORK/result
    "write_case_$lang"
    # 容器内 /judge/work 由 entrypoint 固定
    docker run --rm \
        -v $WORK/src:/judge/work/src \
        -v $WORK/testcases:/judge/work/testcases \
        -v $WORK/result:/judge/work/result \
        -v $WORK/meta.json:/judge/work/meta.json:ro \
        "$img" >/dev/null 2>&1
    local rc=$?
    if [[ $rc -ne 0 ]]; then
        echo "  FAIL  $img  docker exit=$rc"
        fail=$((fail+1))
        return
    fi
    local result=$(jq -r .result $WORK/result/summary.json 2>/dev/null || echo "no-summary")
    if [[ "$result" == "AC" ]]; then
        echo "  OK    $img  AC"
        pass=$((pass+1))
    else
        echo "  FAIL  $img  result=$result"
        cat $WORK/result/summary.json 2>/dev/null | head -10 | sed 's/^/        /'
        fail=$((fail+1))
    fi
}

# ---------- 跑一个 image，期望 TLE ----------
run_tle() {
    local img="$1"
    local lang="$2"
    rm -rf $WORK/* && mkdir -p $WORK/src $WORK/testcases $WORK/result
    cat > $WORK/src/main.* 2>/dev/null <<'EOF'
#include <stdio.h>
int main() { while(1){} return 0; }
EOF
    # 通用死循环文件（每种语言都接受同名 .扩展）
    case "$lang" in
        cpp) cat > $WORK/src/main.cpp <<'EOF'
#include <cstdio>
int main() { while(1){} return 0; }
EOF
;;
        c) cat > $WORK/src/main.c <<'EOF'
#include <stdio.h>
int main(void) { while(1){} return 0; }
EOF
;;
        java) cat > $WORK/src/Main.java <<'EOF'
public class Main { public static void main(String[] a){ while(true){} } }
EOF
;;
        python) cat > $WORK/src/main.py <<'EOF'
while True: pass
EOF
;;
        go) cat > $WORK/src/main.go <<'EOF'
package main
func main() { for {} }
EOF
;;
    esac
    cat > $WORK/meta.json <<EOF
{"language":"$lang","time_limit_ms":500,"memory_limit_mb":128,"output_limit_mb":64}
EOF
    echo "" > $WORK/testcases/1.in
    echo "" > $WORK/testcases/1.out
    docker run --rm \
        -v $WORK/src:/judge/work/src \
        -v $WORK/testcases:/judge/work/testcases \
        -v $WORK/result:/judge/work/result \
        -v $WORK/meta.json:/judge/work/meta.json:ro \
        "$img" >/dev/null 2>&1
    local result=$(jq -r .result $WORK/result/summary.json 2>/dev/null || echo "no-summary")
    if [[ "$result" == "TLE" ]]; then
        echo "  OK    $img  TLE"
        pass=$((pass+1))
    else
        echo "  FAIL  $img  expected TLE, got $result"
        cat $WORK/result/summary.json 2>/dev/null | head -10 | sed 's/^/        /'
        fail=$((fail+1))
    fi
}

# ---------- main ----------
echo "==> AC 测试（每种语言 hello world 加法）"
for pair in "judge-cpp:1.0 cpp" "judge-c:1.0 c" "judge-java:1.0 java" "judge-python:1.0 python" "judge-go:1.0 go"; do
    read -r img lang <<< "$pair"
    run_ac "$img" "$lang"
done

echo
echo "==> TLE 测试（每种语言死循环）"
for pair in "judge-cpp:1.0 cpp" "judge-c:1.0 c" "judge-java:1.0 java" "judge-python:1.0 python" "judge-go:1.0 go"; do
    read -r img lang <<< "$pair"
    run_tle "$img" "$lang"
done

echo
echo "==> 结果:  pass=$pass  fail=$fail"
[[ $fail -eq 0 ]] && exit 0 || exit 1
