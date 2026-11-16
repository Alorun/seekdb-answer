#!/usr/bin/env bash
# Full local admission + correctness gate for MiniVectorDB.
#
# Runs every check that has caught a real failure in this repository's history
# (see docs/FAILURE_LEDGER.md section 3) against one binary/build tree, and
# fails fast.  The official public package (if present at $OFFICIAL_DIR) is run
# through the official JSONL driver, which is the closest local proxy for the
# formal MySQL admission gate.
#
# Usage:
#   bash scripts/full_gate.sh                 # uses ./build/minivectordb
#   BIN=./build-asan/minivectordb bash scripts/full_gate.sh --no-build
#   OFFICIAL_DIR=/tmp/protocol_final bash scripts/full_gate.sh
#
# Exit code 0 only if every gate passed.
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=${BIN:-$ROOT/build/minivectordb}
OFFICIAL_DIR=${OFFICIAL_DIR:-/tmp/protocol_final}
SEED=${SEED:-20261117}
DO_BUILD=1
[ "${1:-}" = "--no-build" ] && DO_BUILD=0
FAILED=0
PASSED=0

step() {
  local name=$1
  shift
  local start
  start=$(date +%s)
  if "$@" > /tmp/full_gate_step.log 2>&1; then
    local dur
    dur=$(( $(date +%s) - start ))
    echo "PASS  ${name}  (${dur}s)"
    PASSED=$((PASSED + 1))
  else
    echo "FAIL  ${name}"
    tail -25 /tmp/full_gate_step.log | sed 's/^/      /'
    FAILED=$((FAILED + 1))
  fi
}

if [ "$DO_BUILD" = 1 ]; then
  step "release build" bash -c 'cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j"$(nproc)"'
fi
[ -x "$BIN" ] || { echo "FAIL  binary missing: $BIN"; exit 1; }

step "public 20 cases" python3 tests/run_public_cases.py --bin "$BIN"
step "sql edges 158x2" python3 tests/run_sql_edges.py
step "recall uniform" python3 tests/run_recall.py --bin "$BIN" --rows 5000 --queries 30
step "recall clustered" python3 tests/run_recall.py --bin "$BIN" --rows 5000 --queries 30 --clustered
step "filter differential 190" python3 tests/run_filter_diff.py
step "marker cases 20" python3 tests/run_marker_cases.py
step "semantics fixes" python3 tests/run_semantics_fixes.py
step "checkpoint 10k/50k" python3 tests/run_checkpoint.py --bin "$BIN"
step "crash fuzz 150" python3 tests/run_crash_fuzz.py "$BIN" --iterations 150 --seed "$SEED"
step "ann clustered 3-phase" python3 tests/run_ann_clustered.py --bin "$BIN"
step "crc self-check" bash -c 'g++ -O2 -std=c++20 -Isrc tests/crc_check.cpp src/common.cpp -o /tmp/crc_check && /tmp/crc_check'
step "vector literal self-check" bash -c 'g++ -O2 -std=c++20 -Isrc tests/parse_vector_check.cpp src/common.cpp -o /tmp/pv_check && /tmp/pv_check'

if [ -f "$OFFICIAL_DIR/protocol_check/run.py" ]; then
  rm -rf /tmp/full_gate_official
  step "official protocol_check 20" python3 "$OFFICIAL_DIR/protocol_check/run.py" \
    --command "[\"$BIN\",\"--data-dir\",\"{data_dir}\"]" \
    --package "$OFFICIAL_DIR/mysqltest" --output /tmp/full_gate_official
else
  echo "SKIP  official protocol_check (package not at $OFFICIAL_DIR)"
fi

echo
echo "gates passed: $PASSED  failed: $FAILED"
[ "$FAILED" = 0 ] || exit 1
