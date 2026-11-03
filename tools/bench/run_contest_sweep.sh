#!/usr/bin/env bash
# One-command contest sweep: 128-d (real SIFT if present, else synthetic) and
# 256-d synthetic, each over a list of graph-build configurations, followed by a
# verdict that says whether the 3x target is reachable from the graph build.
#
# Usage:
#   tools/bench/run_contest_sweep.sh \
#     --observer build_release/src/observer/seekdb \
#     --bridge /work/bridge/libseekdb_cuvs_bridge.so \
#     --work /work/cagra-sweep
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

OBSERVER=""
BRIDGE=""
WORK=/work/cagra-sweep
PORT=2991
ROWS=250000
NQ=4096
MEMORY=32G
SIFT_BASE=""
SIFT_QUERY=""
CONFIGS="current:100:256:384:128 floor:4:64:64:64 it50:50:256:384:128 it30:30:256:384:128 it30d192:30:192:256:128 it20d192:20:192:256:128"
SKIP_SIFT=0
SKIP_SYN=0
SYN_DIM=256

while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --work) WORK=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --nq) NQ=$2; shift 2 ;;
    --memory) MEMORY=$2; shift 2 ;;
    --sift-base) SIFT_BASE=$2; shift 2 ;;
    --sift-query) SIFT_QUERY=$2; shift 2 ;;
    --configs) CONFIGS=$2; shift 2 ;;
    --syn-dim) SYN_DIM=$2; shift 2 ;;
    --skip-sift) SKIP_SIFT=1; shift ;;
    --skip-syn) SKIP_SYN=1; shift ;;
    -h|--help) awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print } else { exit } }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -x "$OBSERVER" ]] || { echo "--observer must be executable" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "--bridge must exist" >&2; exit 2; }
mkdir -p "$WORK"

detect_sift()
{
  if [[ -n "$SIFT_BASE" ]]; then
    return 0
  fi
  local candidate
  for candidate in /work/datasets/sift_base.fbin /work/bench/datasets/sift_base.fbin \
                   /work/datasets/sift/sift_base.fbin; do
    if [[ -f "$candidate" ]]; then
      SIFT_BASE=$candidate
      for q in "$(dirname "$candidate")/sift_query.fbin" "$(dirname "$candidate")/query.fbin"; do
        [[ -f "$q" ]] && SIFT_QUERY=$q && break
      done
      echo "[sweep] using real SIFT: $SIFT_BASE ${SIFT_QUERY:-<no query fbin>}"
      return 0
    fi
  done
  echo "[sweep] no SIFT fbin found; 128-d will use synthetic clustered data"
  return 1
}

run_suite()
{
  local tag=$1 data=$2 port=$3 abs_min=$4
  echo
  echo "================ $tag (data=$data) ================"
  "$HERE/cagra_build_bench.sh" \
    --observer "$OBSERVER" \
    --bridge "$BRIDGE" \
    --data-dir "$data" \
    --base-dir "$WORK/store-$tag" \
    --evidence "$WORK/evidence-$tag" \
    --port "$port" \
    --memory "$MEMORY" \
    --skip-gen \
    --abs-min "$abs_min" \
    --configs "$CONFIGS" 2>&1 | tee "$WORK/run-$tag.log"
  cp "$WORK/evidence-$tag/summary.tsv" "$WORK/summary-$tag.tsv"
}

# ---------------------------------------------------------------- 128-d suite --
if [[ "$SKIP_SIFT" == "0" ]]; then
  SIFT_DATA=$WORK/data-sift250k
  if [[ -f "$SIFT_DATA/meta.json" ]]; then
    echo "[sweep] reusing $SIFT_DATA"
  elif detect_sift; then
    local -a qargs=()
    [[ -n "$SIFT_QUERY" ]] && qargs=(--query-fbin "$SIFT_QUERY")
    python3 "$HERE/gen_bench_data.py" --out "$SIFT_DATA" --rows "$ROWS" --nq "$NQ" \
      --base-fbin "$SIFT_BASE" "${qargs[@]}"
  else
    python3 "$HERE/gen_bench_data.py" --out "$SIFT_DATA" --rows "$ROWS" --dim 128 \
      --nq "$NQ" --kind clustered
  fi
  if ! run_suite sift128 "$SIFT_DATA" "$PORT" 0.95,0.95; then
    echo "[sweep] 128-d suite FAILED; continuing" >&2
  fi
fi

# ---------------------------------------------------------------- 256-d suite --
if [[ "$SKIP_SYN" == "0" ]]; then
  SYN_DATA=$WORK/data-syn$SYN_DIM
  if [[ ! -f "$SYN_DATA/meta.json" ]]; then
    python3 "$HERE/gen_bench_data.py" --out "$SYN_DATA" --rows "$ROWS" --dim "$SYN_DIM" \
      --nq "$NQ" --kind clustered
  fi
  if ! run_suite "syn$SYN_DIM" "$SYN_DATA" "$((PORT + 1))" 0.90,0.85; then
    echo "[sweep] 256-d suite FAILED; continuing" >&2
  fi
fi

# -------------------------------------------------------------------- verdict --
python3 - "$WORK" "$ROWS" <<'PY'
import pathlib, sys

work = pathlib.Path(sys.argv[1])
# Reference frozen-baseline medians divided by 3: SIFT ~23s, Hidden ~30s.
TARGETS = {"sift128": 7700.0, "syn256": 10000.0}

def read(tag):
    path = work / f"summary-{tag}.tsv"
    if not path.exists():
        return []
    lines = [l for l in path.read_text().splitlines() if l.strip()]
    head = lines[0].split("\t")
    return [dict(zip(head, l.split("\t"))) for l in lines[1:]]

print()
print("=" * 78)
for tag in ("sift128", "syn256"):
    rows = read(tag)
    TARGET = TARGETS.get(tag, 7700.0)
    if not rows:
        continue
    by = {r["config"]: r for r in rows}
    cur, floor = by.get("current"), by.get("floor")
    print(f"[{tag}]")
    for r in rows:
        print(f"  {r['config']:<12} build={int(r['build_ms']):>6}ms "
              f"gpu={float(r['nnd_s']) + float(r['sort_s']) + float(r['prune_s']):>6.2f}s "
              f"recall={r['recall10']}/{r['recall100']} qps={r['qps10']}/{r['qps100']} {r['gate']}")
    if cur and floor:
        cur_ms, floor_ms = int(cur["build_ms"]), int(floor["build_ms"])
        gpu_ms = cur_ms - floor_ms
        print(f"  -> DDL/persist floor ~= {floor_ms}ms, GPU graph build ~= {gpu_ms}ms, "
              f"target build <= {int(TARGET)}ms")
        if floor_ms >= TARGET:
            print(f"  -> VERDICT: the non-GPU floor already exceeds the 3x budget; "
                  f"graph tuning alone cannot reach 3x, the DDL/persistence path must be cut "
                  f"by ~{floor_ms - TARGET}ms.")
        else:
            allowed = TARGET - floor_ms
            print(f"  -> VERDICT: graph build may take at most ~{allowed}ms for 3x "
                  f"(currently ~{gpu_ms}ms, i.e. needs ~{gpu_ms / max(allowed, 1):.1f}x less).")
    print()
print("=" * 78)
PY

echo "[sweep] summaries: $WORK/summary-sift128.tsv $WORK/summary-syn256.tsv"
