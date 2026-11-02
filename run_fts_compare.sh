#!/usr/bin/env bash
set -Eeuo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

REPORT_DIR="$ROOT/reports/fts"
BASE_DIR="/home/hahaha/seekdb-tests/fts-after"
PORT=2881
BINARY="$ROOT/build_release/src/observer/seekdb"
SDB="$ROOT/.github/script/seekdb/sdb.py"
BENCH="$ROOT/tools/benchmark/fts_large_bench.sh"
COMPARE="/home/hahaha/fts_bench_compare.py"

mkdir -p "$REPORT_DIR" /home/hahaha/seekdb-tests

# 检查基线报告
if [[ ! -s "$REPORT_DIR/baseline.txt" ]]; then
    echo "ERROR: baseline.txt 不存在，请先生成优化前基线报告"
    exit 1
fi

# 检查分支
BRANCH="$(git branch --show-current)"
if [[ "$BRANCH" != "indextuning" ]]; then
    echo "ERROR: 当前分支是 $BRANCH，请先切换到 indextuning"
    exit 1
fi

# 停止指定目录对应的旧实例
stop_instance() {
    local dir="$1"
    local pidfile pid cmd

    for pidfile in "$dir/run/observer.pid" "$dir/run/seekdb.pid"; do
        [[ -f "$pidfile" ]] || continue
        pid="$(cat "$pidfile" 2>/dev/null || true)"
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        cmd="$(ps -p "$pid" -o args= 2>/dev/null || true)"

        if [[ "$cmd" == *"$dir"* && "$cmd" == *seekdb* ]]; then
            echo "Stopping SeekDB PID=$pid"
            kill -TERM "$pid" 2>/dev/null || true

            for _ in {1..30}; do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done

            if kill -0 "$pid" 2>/dev/null; then
                echo "Force stopping PID=$pid"
                kill -KILL "$pid" 2>/dev/null || true
            fi
        fi
    done
}

cleanup() {
    stop_instance "$BASE_DIR"
}
trap cleanup EXIT INT TERM

echo "========== 1. Build =========="
./build.sh release --make -j"$(nproc)"

[[ -x "$BINARY" ]] || {
    echo "ERROR: SeekDB binary not found"
    exit 1
}

echo "========== 2. Stop old instances =========="
stop_instance "$BASE_DIR"
stop_instance "/tmp/seekdb-fts-after"

rm -rf "$BASE_DIR"

echo "========== 3. Start SeekDB =========="
python3 "$SDB" start \
    --binary "$BINARY" \
    --base-dir "$BASE_DIR" \
    --port "$PORT"

python3 "$SDB" wait-ready \
    --base-dir "$BASE_DIR" \
    --port "$PORT" \
    --client mysql \
    --timeout 180

echo "========== 4. Run benchmark =========="

# 防止读取上一次的旧报告
: > "$REPORT_DIR/indextuning.txt"

timeout 20m env \
    LABEL=indextuning \
    ROWS=20000 \
    BATCH=500 \
    ROUNDS=3000 \
    QUERY_ROUNDS=200 \
    SAMPLES=3 \
    WARMUP=30 \
    SKIP_LOAD=0 \
    OUTPUT="$REPORT_DIR/indextuning.txt" \
    bash "$BENCH" 2>&1 | tee "$REPORT_DIR/indextuning-run.log"

[[ -s "$REPORT_DIR/indextuning.txt" ]] || {
    echo "ERROR: benchmark report was not generated"
    exit 1
}

echo "========== 5. Compare =========="

python3 "$COMPARE" \
    "$REPORT_DIR/baseline.txt" \
    "$REPORT_DIR/indextuning.txt"

echo "========== DONE =========="
echo "Baseline: $REPORT_DIR/baseline.txt"
echo "Current:  $REPORT_DIR/indextuning.txt"
echo "Log:      $REPORT_DIR/indextuning-run.log"
