# CAGRA build/query benchmark (`lib=cuvs`)

Development-only harness for the AMD hipVS contest task. It reproduces the
scored path end to end on a GPU host:

1. import a 250K-row table (untimed, like the contest `LOAD DATA`),
2. time `CREATE VECTOR INDEX ... lib=cuvs` from a cold index,
3. time the first fixed query (the contest counts it into the build),
4. run `nq=4096` single-row SQL queries for Top-10 and Top-100,
5. report QPS, Recall against exact ground truth, and the GPU-build phase split
   taken from the observer log.

Why: `build=1.108x` does not say whether the remaining time is the GPU CAGRA
graph build or the seekdb DDL/persistence path around it. Without that split any
further optimisation is a guess, and each contest submission is expensive.

## 1. Build (official commands, in the GPU container)

```bash
export HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON \
  -DOB_USE_LLD=ON \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

## 2. Data

Real SIFT is the only way to judge the `SIFT250K` gate; the Hidden data is not
public, so 256-d sweeps use synthetic data and only the *relative* build/recall
trend is meaningful.

```bash
# preferred: real SIFT (SIFT1M fbin format: int32 n, int32 dim, float32 data)
python3 tools/bench/gen_bench_data.py --out /work/benchdata/sift250k \
  --base-fbin /work/datasets/sift_base.fbin \
  --query-fbin /work/datasets/sift_query.fbin --rows 250000 --nq 4096

# fallback: synthetic clustered data (works for any --dim)
python3 tools/bench/gen_bench_data.py --out /work/benchdata/syn256 \
  --rows 250000 --dim 256 --nq 4096
```

The generator writes `base.f32`, `query.f32`, `gt10.i32`, `gt100.i32`,
`load.tsv` and `first_query.txt`. Values are written in a format that round-trips
float32 exactly, so the recall numbers are not biased by text formatting.

## 3. Sweep

Every `--configs` entry is `[label:]iterations:degree:internal_degree:final_degree[:search_itopk[:search_width]]`
and is applied by restarting the observer with `SK_CUVS_*` environment
overrides. Omitted values keep the shipped defaults (`100:256:384:128`,
`itopk=512`, `width=4`), so the unmodified build is measured by the `current`
entry.

```bash
tools/bench/cagra_build_bench.sh \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --data-dir /work/benchdata/sift250k \
  --port 2991 --memory 32G \
  --configs "current:100:256:384:128 floor:4:64:64:64 \
              it50:50:256:384:128 it30:30:256:384:128 \
              it30d192:30:192:256:128 it20d192:20:192:256:128"
```

`--abs-min 0.90,0.85` marks the 256-d Hidden absolute lower bounds in the `gate`
column (`0.95,0.95` is the SIFT default).

The `floor` entry is the important one: with a 4-iteration graph the GPU build
cost collapses, so `build_ms(floor)` is the non-GPU floor of the whole path
(scan + per-row append + snapshot write + first-query reload). Then

```text
GPU graph build  ~= build_ms(current) - build_ms(floor)
DDL/persist path ~= build_ms(floor)
```

For a 3x win on SIFT the total must drop below ~7.7 s. If `build_ms(floor)` is
already close to that number, graph tuning alone cannot get there and the DDL
path has to be attacked; if the floor is small, the graph build is the target.

## 4. Reading the output

`summary.tsv` columns:

| column | meaning |
| --- | --- |
| `create_ms` | wall time of `CREATE VECTOR INDEX` (GPU build + snapshot write) |
| `firstq_ms` | first fixed query (part of the contest build time) |
| `build_ms` | `create_ms + firstq_ms`, the number to compare with the contest |
| `nnd_s`, `sort_s`, `prune_s`, `attach_s` | GPU graph build phases (`SEEKDB_GPU_BUILD`) |
| `serialize_s`, `stream_s` | cuVS snapshot write / LOB stream write (`OBVSAG`) |
| `recall10`, `recall100` | recall over 4096 queries against exact ground truth |
| `qps10`, `qps100` | queries per second for the single-row SQL workload |
| `gate` | recall vs `--abs-min` |

Per-config observer logs are kept in `evidence/*-observer-phase.log`; the raw
per-query ids are in `evidence/*-q10.tsv` / `*-q100.tsv`.

## 5. Knobs (all default to the shipped values)

| variable | default | effect |
| --- | --- | --- |
| `SK_CUVS_NND_ITER` | 100 | NN-descent iterations (linear in build time) |
| `SK_CUVS_NND_DEGREE` | 256 | kNN list kept per node |
| `SK_CUVS_NND_INTERNAL_DEGREE` | 384 | NN-descent intermediate degree |
| `SK_CUVS_FINAL_DEGREE` | 128 | CAGRA graph degree after pruning (also snapshot size, search speed) |
| `SK_CUVS_SEARCH_ITOPK` | 512 | search candidate list (recall vs QPS) |
| `SK_CUVS_SEARCH_WIDTH` | 4 | CAGRA search width (recall vs QPS) |

Recall can be bought back after a cheaper graph build with `itopk`/`width`, paid
for out of the query-speed headroom (`query=1.689x`, full marks at `1.5x`).

## 6. Caveats

- The contest compares against the frozen V5 baseline in the same round; this
  harness only measures the candidate. Use the `current` entry as the local
  reference and keep a margin on the `baseline - 0.005` gate.
- Synthetic data does not reproduce Hidden250K recall. Treat 256-d numbers as a
  trend, and confirm the final choice in the contest evaluation.
- Reset `--base-dir` between unrelated experiments; it holds the observer store.
