#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
BIN="${BIN:-./build/minivectordb}"
cmake --build build -j"$(nproc)"
python3 tests/run_public_cases.py --bin "$BIN"
python3 tests/run_sql_edges.py
python3 tests/run_recall.py --bin "$BIN" --rows 5000 --queries 30
python3 tests/run_recall.py --bin "$BIN" --rows 5000 --queries 30 --clustered
python3 tests/run_crash_fuzz.py "$BIN" --iterations 100 --seed 20261114
python3 tests/run_checkpoint.py --bin "$BIN"
python3 tests/run_marker_cases.py
python3 scripts/release_gate.py --require-conversation
echo "pre-submit check: PASS"
