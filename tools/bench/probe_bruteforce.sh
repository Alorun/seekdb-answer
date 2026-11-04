#!/usr/bin/env bash
# Probe: exact-kNN build on the submission default + the fixed-cost floor.
#
# The bundle's observer predates the bulk-append change, so it is NOT rebuilt
# here (a full observer rebuild would need the matching source tree).  Instead:
#   1) normal run: the exact-kNN build on real SIFT250K and 256d;
#   2) floor run: a ring graph with an almost-free GPU stage, which measures the
#      DDL scan + snapshot + commit part of the timed build.
# The floor minus the tiny GPU stage is the fixed cost; the 12.99 report's
# 1.545x -> 1.731x step says bulk append removes about 2.1s of it on the eval.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC=${SEEKDB_SRC:-/work/seekdb-bundle/seekdb}
BR=${SEEKDB_BRIDGE:-/work/bridge/libseekdb_cuvs_bridge.so}
W=/work/bf-run3/sweep
W2=/work/bf-run3
PORT=3041
say() { echo "[$(date +%H:%M:%S)] $*"; }

[[ -d "$SRC" ]] || { say "FAILED: no source at $SRC"; exit 1; }
[[ -x "$SRC/build_release/src/observer/seekdb" ]] || { say "FAILED: no prebuilt observer"; exit 1; }

SIFT=""
for C in /work/cagra-sweep /work/algo-probe-run/sweep /work/sweep /work/bench /work/bf-run2/sweep /work/datasets; do
  [[ -z "$SIFT" && -f "$C/data-sift250k/meta.json" ]] && SIFT="$C/data-sift250k"
done
[[ -n "$SIFT" ]] || { say "FAILED: no data-sift250k"; exit 1; }
mkdir -p "$W" "$W2"
[[ -L "$W/data-sift250k" ]] && rm -f "$W/data-sift250k"
ln -sfn "$SIFT" "$W/data-sift250k"
say "sift=$SIFT"

BUILDER=$HERE/custom_builder.hip
BRIDGEC=$HERE/seekdb_cuvs_bridge.c
GRAPH=$HERE/graph_core.cuh
for f in "$BUILDER" "$BRIDGEC" "$GRAPH"; do
  [[ -f "$f" ]] || { say "FAILED: missing $f"; exit 1; }
done

export HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm
rm -f "$W2"/timing-*.log "$W2"/sweep-*.log "$W2"/bridge.log 2>/dev/null

say "== install bridge sources =="
cp "$BUILDER" "$SRC/tools/hipvs_bridge/kernel_build/custom_builder.hip" || exit 1
cp "$BRIDGEC" "$SRC/tools/hipvs_bridge/seekdb_cuvs_bridge.c" || exit 1
cp "$GRAPH"   "$SRC/tools/hipvs_bridge/kernel_build/vendor/cpp/src/neighbors/detail/cagra/graph_core.cuh" || exit 1
sha256sum "$SRC/tools/hipvs_bridge/kernel_build/custom_builder.hip" \
          "$SRC/tools/hipvs_bridge/seekdb_cuvs_bridge.c" \
          "$SRC/tools/hipvs_bridge/kernel_build/vendor/cpp/src/neighbors/detail/cagra/graph_core.cuh" \
  | sed 's/^/  installed /'

say "== rebuild bridge =="
( cd "$SRC" && tools/hipvs_bridge/build.sh "$BR" ) >"$W2/bridge.log" 2>&1 \
  || { say "FAILED: bridge build"; tail -n 40 "$W2/bridge.log"; exit 1; }
say "bridge ok"

run_sweep() {
  local tag=$1 port=$2 cfg=$3 extra=$4
  say "== sweep $tag ($cfg) =="
  ( cd "$SRC" && tools/bench/run_contest_sweep.sh \
      --observer build_release/src/observer/seekdb --bridge "$BR" \
      --work "$W" --memory 32G --port "$port" \
      --extra-env "$extra" --configs "$cfg" ) >"$W2/sweep-$tag.log" 2>&1
  say "---- $tag ----"
  grep -h "\[bench\].*build_ms=" "$W2/sweep-$tag.log" | tail -4
}

run_sweep normal "$PORT"        "sub:0:256:384:128" "SK_CUVS_TIMING_FILE=$W2/timing-normal.log"
run_sweep floor  "$((PORT+10))" "floor:0:128:128:128" "SK_CUVS_TIMING_FILE=$W2/timing-floor.log,SK_CUVS_BUILD_ALGO=floor"

say "== phase lines =="
for f in "$W2/timing-normal.log" "$W2/timing-floor.log"; do
  [[ -f "$f" ]] || continue
  echo "== $f"
  grep -h "SEEKDB_GPU_BUILD total_sec\|SEEKDB_BF_BUILD\|SEEKDB_BRIDGE_BUILD\|SEEKDB_BRIDGE_SERIALIZE\|SEEKDB_CUVS_APPEND" "$f" | tail -5
done
echo "---- summaries ----"
for f in "$W"/summary-*.tsv; do [[ -f "$f" ]] && { echo "== $f"; cat "$f"; }; done
say "ALL DONE"
