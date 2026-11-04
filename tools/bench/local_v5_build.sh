#!/usr/bin/env bash
# Build a V5 contest seekdb from this tree on the notebook GPU image, then run
# the contest CAGRA build benchmark (normal + fixed-cost floor) on 250K data.
#
# Usage:  bash tools/bench/local_v5_build.sh
#   expects the notebook image: /opt/hipvs /opt/rocm /opt/seekdb-deps-cache,
#   and about 1.5 TB free on / for the build directory.
#
# Logs:  /workspace/run/{dep,bridge,cmake,build,bench}.log
set -u
SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
RUN=${RUN_DIR:-/run-bench}
BUILD=${BUILD_DIR:-/build_release}
BR=$RUN/bridge/libseekdb_cuvs_bridge.so
OBS=$BUILD/src/observer/seekdb
DATA=$RUN/data
export HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm DEP_CACHE_DIR=/opt/seekdb-deps-cache
say() { echo "[$(date +%H:%M:%S)] $*"; }

mkdir -p "$RUN/bridge" "$DATA"
say "src=$SRC build=$BUILD"

say "== dependencies (shared cache) =="
( cd "$SRC" && bash deps/init/dep_create.sh ) >"$RUN/dep.log" 2>&1 \
  || { say "FAILED: dep_create"; tail -n 30 "$RUN/dep.log"; exit 1; }
say "deps ok"

say "== bridge =="
( cd "$SRC" && tools/hipvs_bridge/build.sh "$BR" ) >"$RUN/bridge.log" 2>&1 \
  || { say "FAILED: bridge build"; tail -n 40 "$RUN/bridge.log"; exit 1; }
say "bridge ok: $(ls -l "$BR" | awk '{print $5}') bytes"

say "== cmake configure =="
cmake -S "$SRC" -B "$BUILD" -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB="$BR" >"$RUN/cmake.log" 2>&1 \
  || { say "FAILED: cmake"; tail -n 40 "$RUN/cmake.log"; exit 1; }
say "cmake ok"

say "== build seekdb (this takes 25-40 min) =="
cmake --build "$BUILD" --target seekdb -j24 >"$RUN/build.log" 2>&1 \
  || { say "FAILED: build"; tail -n 50 "$RUN/build.log"; exit 1; }
say "observer ok: $(ls -l "$OBS" | awk '{print $5}') bytes"

gen() {
  local out=$1 dim=$2
  [[ -f "$out/meta.json" ]] && { say "reuse $out"; return 0; }
  say "== generate $out (250K x $dim) =="
  python3 "$SRC/tools/bench/gen_bench_data.py" --out "$out" --rows 250000 --dim "$dim" --nq 4096 --kind clustered \
    >"$RUN/gen-$(basename "$out").log" 2>&1 \
    || { say "FAILED: gen $out"; tail -n 20 "$RUN/gen-$(basename "$out").log"; exit 1; }
}
gen "$DATA/data-sift250k" 128
gen "$DATA/data-syn256" 256

run() {
  local tag=$1 dir=$2 port=$3 cfg=$4 extra=$5
  say "== bench $tag ($cfg) =="
  "$SRC/tools/bench/cagra_build_bench.sh" \
    --observer "$OBS" --bridge "$BR" \
    --data-dir "$dir" --base-dir "$RUN/store-$tag" --evidence "$RUN/ev-$tag" \
    --port "$port" --memory 32G --datafile-size 6G --skip-gen --configs "$cfg" \
    --extra-env "$extra" >"$RUN/bench-$tag.log" 2>&1
  grep -h "\[bench\].*build_ms=" "$RUN/bench-$tag.log" | tail -2
}

run normal-sift "$DATA/data-sift250k" 3061 "sub:0:256:384:128"  "SK_CUVS_TIMING_FILE=$RUN/t-normal-sift.log"
run floor-sift  "$DATA/data-sift250k" 3071 "floor:0:128:128:128" "SK_CUVS_TIMING_FILE=$RUN/t-floor-sift.log,SK_CUVS_BUILD_ALGO=floor"
run normal-256  "$DATA/data-syn256"   3081 "sub:0:256:384:128"  "SK_CUVS_TIMING_FILE=$RUN/t-normal-256.log"
run floor-256   "$DATA/data-syn256"   3091 "floor:0:128:128:128" "SK_CUVS_TIMING_FILE=$RUN/t-floor-256.log,SK_CUVS_BUILD_ALGO=floor"

say "== timing split =="
for f in "$RUN"/t-*.log; do
  [[ -f "$f" ]] || continue
  echo "== $f"
  grep -h "SEEKDB_BF_BUILD\|SEEKDB_GPU_BUILD total_sec\|SEEKDB_BRIDGE_BUILD\|SEEKDB_BRIDGE_SERIALIZE\|SEEKDB_CUVS_APPEND" "$f" | tail -5
done
say "ALL DONE"
