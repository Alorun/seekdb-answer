#!/usr/bin/env bash
# End-to-end CAGRA build/query benchmark for seekdb lib=cuvs.
#
# Measures, for every graph-build configuration in --configs:
#   * CREATE VECTOR INDEX wall time (cold DDL, includes GPU build + snapshot write)
#   * first fixed query wall time (counted as part of the contest build time)
#   * nq x Top-10 / Top-100 QPS and Recall against exact ground truth
#   * the GPU build phase split from the observer log
#     (SEEKDB_GPU_BUILD nnd/sort/prune/attach, OBVSAG serialize build/snapshot/stream)
#
# Usage:
#   tools/bench/cagra_build_bench.sh \
#     --observer /work/seekdb/build_release/src/observer/seekdb \
#     --bridge /work/bridge/libseekdb_cuvs_bridge.so \
#     --data-dir /work/benchdata/sift250k --port 2991 \
#     --configs "current:100:256:384:128 floor:4:64:64:64"
#
# The tuning knobs are read by the bridge at build/search time, so every config
# is applied by restarting the observer with different SK_CUVS_* env vars.
set -Eeuo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

OBSERVER=""
BRIDGE=""
DATA_DIR=""
PORT=2981
BASE_DIR="/tmp/seekdb-cagra-bench"
EVIDENCE=""
ROWS=250000
DIM=128
NQ=4096
MEMORY=16G
DATAFILE=40G
CONFIGS="current:100:256:384:128 floor:4:64:64:64"
SKIP_GEN=0
DB=bench
TABLE=items
INDEX_NAME=idx_items
GENERATE_ARGS=""
ABS_MIN="0.95,0.95"
EXTRA_ENV=""
PRE_SQL=""

usage()
{
  awk 'NR > 1 { if (/^#/) { sub(/^# ?/, ""); print } else { exit } }' "${BASH_SOURCE[0]}"
  exit "${1:-2}"
}

while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --data-dir) DATA_DIR=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --evidence) EVIDENCE=$2; shift 2 ;;
    --rows) ROWS=$2; GENERATE_ARGS+=" --rows $2"; shift 2 ;;
    --dim) DIM=$2; GENERATE_ARGS+=" --dim $2"; shift 2 ;;
    --nq) NQ=$2; GENERATE_ARGS+=" --nq $2"; shift 2 ;;
    --configs) CONFIGS=$2; shift 2 ;;
    --memory) MEMORY=$2; shift 2 ;;
    --datafile-size) DATAFILE=$2; shift 2 ;;
    --skip-gen) SKIP_GEN=1; shift ;;
    --abs-min) ABS_MIN=$2; shift 2 ;;
    --extra-env) EXTRA_ENV=$2; shift 2 ;;
    --pre-sql) PRE_SQL=$2; shift 2 ;;
    --generate-args) GENERATE_ARGS+=" $2"; shift 2 ;;
    -h|--help) usage 0 ;;
    *) echo "unknown argument: $1" >&2; usage ;;
  esac
done

[[ -n "$OBSERVER" && -x "$OBSERVER" ]] || { echo "--observer must be an executable" >&2; exit 2; }
[[ -n "$BRIDGE" && -f "$BRIDGE" ]] || { echo "--bridge must be a shared library" >&2; exit 2; }
[[ -n "$DATA_DIR" ]] || { echo "--data-dir is required" >&2; exit 2; }
[[ -c /dev/kfd ]] || { echo "/dev/kfd is unavailable" >&2; exit 2; }
command -v mysql >/dev/null || { echo "mysql client not found" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 not found" >&2; exit 2; }

EVIDENCE=${EVIDENCE:-$DATA_DIR/evidence}
mkdir -p "$BASE_DIR" "$EVIDENCE"
LOG=$EVIDENCE/observer.log
: > "$LOG"
OBSERVER_PID=""

mysql_cmd()
{
  timeout 1800 mysql --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$PORT" \
    -uroot -A -N -B "$@"
}

now_ms()
{
  date +%s%3N
}

stop_observer()
{
  [[ -n "$OBSERVER_PID" ]] || return 0
  if [[ -d /proc/$OBSERVER_PID ]]; then
    kill -TERM "$OBSERVER_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do
      [[ -d /proc/$OBSERVER_PID ]] || break
      sleep 0.5
    done
    [[ -d /proc/$OBSERVER_PID ]] && kill -KILL "$OBSERVER_PID" 2>/dev/null || true
  fi
  OBSERVER_PID=""
}
trap stop_observer EXIT

wait_ready()
{
  for _ in $(seq 1 240); do
    [[ -d /proc/$OBSERVER_PID ]] || return 1
    if mysql_cmd -e 'select 1' >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done
  return 1
}

start_observer()
{
  local iter=$1 degree=$2 internal=$3 final=$4
  local itopk=${5:-} width=${6:-}
  stop_observer
  local run_dir=$BASE_DIR/store-$PORT
  mkdir -p "$run_dir"
  ulimit -n 65535
  local -a tune_env=(
    "SK_CUVS_NND_ITER=$iter"
    "SK_CUVS_NND_DEGREE=$degree"
    "SK_CUVS_NND_INTERNAL_DEGREE=$internal"
    "SK_CUVS_FINAL_DEGREE=$final"
  )
  [[ -n "$itopk" ]] && tune_env+=("SK_CUVS_SEARCH_ITOPK=$itopk")
  [[ -n "$width" ]] && tune_env+=("SK_CUVS_SEARCH_WIDTH=$width")
  if [[ -n "$EXTRA_ENV" ]]; then
    local -a extra_env=()
    IFS=',' read -r -a extra_env <<< "$EXTRA_ENV"
    local pair
    for pair in "${extra_env[@]}"; do
      [[ -n "$pair" ]] && tune_env+=("$pair")
    done
  fi
  env \
    LD_LIBRARY_PATH="$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib" \
    "${tune_env[@]}" \
    "$OBSERVER" \
    --nodaemon \
    --port "$PORT" \
    --data-dir "$run_dir/store" \
    --base-dir "$run_dir" \
    --parameter __min_full_resource_pool_memory=2147483648 \
    --parameter "memory_budget=$MEMORY" \
    --parameter "datafile_size=$DATAFILE" \
    --parameter cpu_count=24 \
    --parameter local_ip=127.0.0.1 \
    >>"$LOG" 2>&1 &
  OBSERVER_PID=$!
  wait_ready || { echo "observer failed to start on port $PORT (see $LOG)" >&2; exit 1; }
}

sql_first_query()
{
  local literal=$1 limit=$2
  echo "select id from $DB.$TABLE order by l2_distance(v,'$literal') APPROXIMATE limit $limit;"
}

prepare_data()
{
  if [[ "$SKIP_GEN" == "1" && -f "$DATA_DIR/load.tsv" ]]; then
    return 0
  fi
  python3 "$HERE/gen_bench_data.py" --out "$DATA_DIR" $GENERATE_ARGS
}

# ---------------------------------------------------------------- data / SQL --
prepare_data
[[ -f "$DATA_DIR/meta.json" ]] || { echo "missing $DATA_DIR/meta.json" >&2; exit 1; }
read -r DATA_ROWS DATA_DIM DATA_NQ < <(python3 - "$DATA_DIR/meta.json" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1]))
print(meta["rows"], meta["dim"], meta["nq"])
PY
)
echo "[bench] rows=$DATA_ROWS dim=$DATA_DIM nq=$DATA_NQ"

if [[ ! -f "$DATA_DIR/q10.sql" || ! -f "$DATA_DIR/q100.sql" ]]; then
  echo "[bench] building query SQL files"
  python3 - "$DATA_DIR" "$DB" "$TABLE" <<'PY'
import json, pathlib, sys
import numpy as np
data = pathlib.Path(sys.argv[1])
db, table = sys.argv[2], sys.argv[3]
meta = json.loads((data / "meta.json").read_text())
dim, nq = meta["dim"], meta["nq"]
queries = np.fromfile(data / "query.f32", dtype="float32").reshape(nq, dim)
fmt = meta["format"]
for limit, name in ((10, "q10.sql"), (100, "q100.sql")):
    with (data / name).open("w") as out:
        for row in queries:
            literal = ",".join(fmt % value for value in row)
            out.write(f"select id from {db}.{table} order by l2_distance(v,'[{literal}]') APPROXIMATE limit {limit};\n")
PY
fi

FIRST_QUERY=$(cat "$DATA_DIR/first_query.txt")

# ------------------------------------------------------------- one config run --
restart_required=0
SUMMARY=$EVIDENCE/summary.tsv
: > "$SUMMARY"
printf 'config\tcreate_ms\tfirstq_ms\tbuild_ms\tnnd_s\tsort_s\tprune_s\tattach_s\tserialize_s\tstream_s\trecall10\trecall100\tqps10\tqps100\tgate\n' >> "$SUMMARY"

config_index=0
for config in $CONFIGS; do
  IFS=':' read -r -a fields <<< "$config"
  itopk=""; width=""
  if [[ ${#fields[@]} -ge 4 && ${#fields[@]} -le 7 ]]; then
    if [[ ${fields[0]} =~ ^[0-9]+$ ]]; then
      label="cfg$config_index"
      iter=${fields[0]}; degree=${fields[1]}; internal=${fields[2]}; final=${fields[3]}
      [[ ${#fields[@]} -ge 5 ]] && itopk=${fields[4]}
      [[ ${#fields[@]} -ge 6 ]] && width=${fields[5]}
    else
      label=${fields[0]}
      iter=${fields[1]}; degree=${fields[2]}; internal=${fields[3]}; final=${fields[4]}
      [[ ${#fields[@]} -ge 6 ]] && itopk=${fields[5]}
      [[ ${#fields[@]} -ge 7 ]] && width=${fields[6]}
    fi
  else
    echo "bad config '$config' (want [label:]iter:degree:internal:final[:itopk[:width]])" >&2
    exit 2
  fi
  echo "[bench] === $label iter=$iter degree=$degree internal=$internal final=$final itopk=${itopk:-default} width=${width:-default}"

  start_observer "$iter" "$degree" "$internal" "$final" "$itopk" "$width"

  if [[ $config_index -eq 0 ]]; then
    # The data import is not part of the contest build timing.
    echo "[bench] importing $DATA_ROWS rows (untimed)"
    mysql_cmd -e "create database if not exists $DB;" >/dev/null
    mysql_cmd -e "drop table if exists $DB.$TABLE;" >/dev/null
    mysql_cmd -e "create table $DB.$TABLE(id bigint primary key, v vector($DATA_DIM));" >/dev/null
    import_start=$(now_ms)
    if ! mysql_cmd -e "load data /*+ parallel(16) */ infile '$DATA_DIR/load.tsv' into table $DB.$TABLE fields terminated by '\t' lines terminated by '\n';" >/dev/null 2>"$EVIDENCE/load-data-error.log"; then
      echo "[bench] LOAD DATA failed (see load-data-error.log); falling back to INSERT"
      if [[ ! -s "$DATA_DIR/insert.sql" ]]; then
        python3 - "$DATA_DIR" "$DB" "$TABLE" <<'PY'
import pathlib, sys
data, db, table = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
src = data / "load.tsv"
dst = data / "insert.sql"
with src.open() as fin, dst.open("w", buffering=1 << 22) as out:
    out.write(f"use {db};\n")
    batch = []
    for line in fin:
        row_id, vector = line.rstrip("\n").split("\t", 1)
        batch.append(f"({row_id},'{vector}')")
        if len(batch) >= 250:
            out.write(f"insert into {table} values " + ",".join(batch) + ";\n")
            batch.clear()
    if batch:
        out.write(f"insert into {table} values " + ",".join(batch) + ";\n")
PY
      fi
      mysql_cmd < "$DATA_DIR/insert.sql" >/dev/null
    fi
    import_ms=$(( $(now_ms) - import_start ))
    count=$(mysql_cmd -e "select count(*) from $DB.$TABLE;")
    [[ "$count" == "$DATA_ROWS" ]] || { echo "imported $count rows, expected $DATA_ROWS" >&2; exit 1; }
    echo "[bench] import_ms=$import_ms"
    mysql_cmd -e "alter system set ob_vector_memory_limit_percentage=30;" >/dev/null || true
  else
    mysql_cmd -e "drop index $INDEX_NAME on $DB.$TABLE;" >/dev/null 2>&1 || true
    mysql_cmd -e "alter system set ob_vector_memory_limit_percentage=30;" >/dev/null || true
  fi

  log_offset=$(($(wc -l < "$LOG") + 1))

  create_start=$(now_ms)
  create_sql="create vector index $INDEX_NAME on $DB.$TABLE(v) with (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);"
  [[ -n "$PRE_SQL" ]] && create_sql="$PRE_SQL; $create_sql"
  if ! mysql_cmd -e "$create_sql" \
      >/dev/null 2>"$EVIDENCE/$label-create.err"; then
    echo "[bench] $label CREATE failed; continuing" >&2
    tail -n 3 "$EVIDENCE/$label-create.err" >&2 || true
    printf '%s\tFAIL\t\tFAIL\t\t\t\t\t\t\t\t\t\t\tFAIL\n' "$label" >> "$SUMMARY"
    config_index=$((config_index + 1))
    continue
  fi
  create_ms=$(( $(now_ms) - create_start ))

  query_start=$(now_ms)
  if ! first_rows=$(mysql_cmd -e "$(sql_first_query "$FIRST_QUERY" 10)"); then
    echo "[bench] $label first query failed; continuing" >&2
    printf '%s\t%s\tFAIL\tFAIL\t\t\t\t\t\t\t\t\t\t\tFAIL\n' "$label" "$create_ms" >> "$SUMMARY"
    config_index=$((config_index + 1))
    continue
  fi
  firstq_ms=$(( $(now_ms) - query_start ))
  first_count=$(printf '%s\n' "$first_rows" | grep -c . || true)
  if [[ "$first_count" -ne 10 ]]; then
    echo "[bench] $label first query returned $first_count rows; continuing" >&2
    printf '%s\t%s\t\tFAIL\t\t\t\t\t\t\t\t\t\t\tFAIL\n' "$label" "$create_ms" >> "$SUMMARY"
    config_index=$((config_index + 1))
    continue
  fi

  build_ms=$((create_ms + firstq_ms))

  q10_start=$(now_ms)
  mysql_cmd < "$DATA_DIR/q10.sql" > "$EVIDENCE/$label-q10.tsv"
  q10_ms=$(( $(now_ms) - q10_start ))
  q100_start=$(now_ms)
  mysql_cmd < "$DATA_DIR/q100.sql" > "$EVIDENCE/$label-q100.tsv"
  q100_ms=$(( $(now_ms) - q100_start ))

  tail -n +"$log_offset" "$LOG" > "$EVIDENCE/$label-observer-phase.log" || true

  read -r nnd_s sort_s prune_s attach_s serialize_s stream_s recall10 recall100 qps10 qps100 < <(
    python3 - "$EVIDENCE/$label-observer-phase.log" "$EVIDENCE/$label-q10.tsv" \
      "$EVIDENCE/$label-q100.tsv" "$DATA_DIR" "$DATA_NQ" "$q10_ms" "$q100_ms" <<'PY'
import json, pathlib, re, sys
log_path, q10_path, q100_path, data_dir, nq, q10_ms, q100_ms = sys.argv[1:8]
nq = int(nq)
_meta = json.loads((pathlib.Path(data_dir) / "meta.json").read_text())
rows = int(_meta["rows"])
q10_ms = int(q10_ms); q100_ms = int(q100_ms)
text = pathlib.Path(log_path).read_text(errors="replace") if pathlib.Path(log_path).exists() else ""

def grab(pattern):
    hit = None
    for line in text.splitlines():
        if pattern in line:
            hit = line
    return hit or ""

nnd = sort = prune = attach = 0.0
match = re.search(r"SEEKDB_GPU_BUILD.*nnd_sec=([0-9.]+) sort_sec=([0-9.]+) "
                  r"prune_reverse_sec=([0-9.]+) attach_sec=([0-9.]+) total_sec=([0-9.]+)", text)
if match:
    nnd, sort, prune, attach = (float(match.group(i)) for i in range(1, 5))

serialize = stream = 0.0
match = re.search(r"serialized GPU CAGRA snapshot.*build_call_us=(\d+).*snapshot_us=(\d+).*stream_write_us=(\d+)", text)
if match:
    serialize = int(match.group(2)) / 1e6
    stream = int(match.group(3)) / 1e6

import numpy as np
data = pathlib.Path(data_dir)
gt10 = np.fromfile(data / "gt10.i32", dtype=np.int32).reshape(nq, 10)
gt100 = np.fromfile(data / "gt100.i32", dtype=np.int32).reshape(nq, 100)

def recall(tsv, gt):
    ids = [int(x) for x in pathlib.Path(tsv).read_text().split()]
    if len(ids) != nq * gt.shape[1]:
        raise SystemExit(f"unexpected result rows in {tsv}: {len(ids)} != {nq * gt.shape[1]}")
    got = np.array(ids, dtype=np.int64).reshape(nq, gt.shape[1])
    if got.min() < 0 or got.max() >= rows:
        raise SystemExit(f"result ids out of range: [{got.min()}, {got.max()}]")
    hits = 0
    for want, have in zip(gt, got):
        hits += len(set(want.tolist()) & set(have.tolist()))
    return hits / (nq * gt.shape[1])

r10 = recall(q10_path, gt10)
r100 = recall(q100_path, gt100)
qps10 = nq / (q10_ms / 1000.0)
qps100 = nq / (q100_ms / 1000.0)
print(f"{nnd:.3f} {sort:.3f} {prune:.3f} {attach:.3f} {serialize:.3f} {stream:.3f} "
      f"{r10:.4f} {r100:.4f} {qps10:.1f} {qps100:.1f}")
PY
  )

  gate=$(awk -v want="$ABS_MIN" -v r10="$recall10" -v r100="$recall100" 'BEGIN {
    split(want, w, ",");
    print (r10 + 0 >= w[1] + 0 && r100 + 0 >= w[2] + 0) ? "PASS" : "BELOW";
  }')
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$label" "$create_ms" "$firstq_ms" "$build_ms" "$nnd_s" "$sort_s" "$prune_s" \
    "$attach_s" "$serialize_s" "$stream_s" "$recall10" "$recall100" "$qps10" "$qps100" "$gate" \
    >> "$SUMMARY"
  echo "[bench] $label build_ms=$build_ms create_ms=$create_ms firstq_ms=$firstq_ms recall10=$recall10 recall100=$recall100 qps10=$qps10 qps100=$qps100 abs_gate=$gate"

  config_index=$((config_index + 1))
done

echo
echo "[bench] summary ($SUMMARY)"
column -t -s $'\t' "$SUMMARY" 2>/dev/null || cat "$SUMMARY"
echo
echo "[bench] raw observer phases: $EVIDENCE/*-observer-phase.log"
