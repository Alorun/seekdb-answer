#!/usr/bin/env bash
# One-command probe for the exact GPU brute-force graph builder.
#
# Installs tools/hipvs_bridge/kernel_build/custom_builder.hip into a seekdb
# source tree, rebuilds the bridge, then measures the scored path (cold
# CREATE VECTOR INDEX + first query, 4096 Top-10/Top-100 queries, exact
# Recall) on real SIFT250K (128d) and on the synthetic 256d suite.
#
# Usage:  tools/bench/probe_bruteforce.sh [degree] [port]
#   degree  graph candidate degree, default 256 (k <= 256 keeps the frozen
#           library's k-selection on its warp-sort path)
#   port    base SQL port, default 3031
#
# The benchmark datasets are reused from an existing sweep work directory;
# nothing is regenerated.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${SEEKDB_SRC:-/work/seekdb-bundle/seekdb}
BR=${SEEKDB_BRIDGE:-/work/bridge/libseekdb_cuvs_bridge.so}
W=/work/bf-run/sweep
DEG=${1:-256}
PORT=${2:-3031}
say() { echo "[$(date +%H:%M:%S)] $*"; }

[[ -d "$SRC" ]] || { say "FAILED: no source at $SRC"; exit 1; }
[[ -x "$SRC/build_release/src/observer/seekdb" ]] || { say "FAILED: no prebuilt observer"; exit 1; }

# Reuse any existing benchmark dataset directories without regenerating them.
SIFT=""; SYN=""
for C in /work/cagra-sweep /work/algo-probe-run/sweep /work/sweep /work/bench /work/datasets; do
  [[ -z "$SIFT" && -f "$C/data-sift250k/meta.json" ]] && SIFT="$C/data-sift250k"
  [[ -z "$SYN"  && -f "$C/data-syn256/meta.json"  ]] && SYN="$C/data-syn256"
done
[[ -n "$SIFT" ]] || { say "FAILED: no data-sift250k/meta.json found"; exit 1; }
mkdir -p "$W"
rm -f "$W/data-sift250k" "$W/data-syn256"
ln -s "$SIFT" "$W/data-sift250k"
[[ -n "$SYN" ]] && ln -s "$SYN" "$W/data-syn256"
say "sift=$SIFT syn=${SYN:-<none>} degree=$DEG port=$PORT"

export HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm
W2=/work/bf-run
mkdir -p "$W2"
rm -f "$W2"/*-timing.log "$W2/timing.log" 2>/dev/null

BUILDER=$HERE/custom_builder.hip
[[ -f "$BUILDER" ]] || BUILDER=$HERE/../hipvs_bridge/kernel_build/custom_builder.hip
[[ -f "$BUILDER" ]] || { say "FAILED: no custom_builder.hip next to this script"; exit 1; }
say "== install custom_builder ($BUILDER), rebuild bridge =="
cp "$BUILDER" "$SRC/tools/hipvs_bridge/kernel_build/custom_builder.hip" || exit 1
( cd "$SRC" && tools/hipvs_bridge/build.sh "$BR" ) >"$W2/bridge.log" 2>&1 \
  || { say "FAILED: bridge build"; tail -n 40 "$W2/bridge.log"; exit 1; }
say "bridge ok"

SKIP=""
[[ -z "$SYN" ]] && SKIP="--skip-syn"
say "== sweep: brute degree=$DEG (sift128${SYN:+ + syn256}) =="
( cd "$SRC" && tools/bench/run_contest_sweep.sh \
    --observer build_release/src/observer/seekdb --bridge "$BR" \
    --work "$W" --memory 32G $SKIP --port "$PORT" \
    --extra-env "SK_CUVS_TIMING_FILE=$W2/timing.log,SK_CUVS_BUILD_ALGO=brute" \
    --configs "bf:0:${DEG}:384:128" ) >"$W2/sweep.log" 2>&1
say "== sweep done =="
echo "---- build_ms / recall ----"
grep -h "\[bench\].*build_ms=" "$W2/sweep.log" | tail -4
echo "---- graph build phases ----"
grep -h "SEEKDB_BF\|SEEKDB_GPU_BUILD\|SEEKDB_BRIDGE_BUILD\|SEEKDB_BRIDGE_SERIALIZE" "$W2/timing.log" 2>/dev/null | tail -20
echo "---- failures ----"
grep -h "FAILED\|failed\|Error\|error" "$W2/timing.log" 2>/dev/null | tail -8
tail -n 6 "$W2/sweep.log"
echo "---- summaries ----"
for f in "$W"/summary-*.tsv; do [[ -f "$f" ]] && { echo "== $f"; cat "$f"; }; done
say "ALL DONE"
