# seekdb × hipVS CAGRA build optimization — contest report

Task: accelerate `CREATE VECTOR INDEX ... lib=cuvs` (FP32, L2, CAGRA on AMD
gfx1100) end to end while keeping Recall, query throughput and resource gates.
All numbers below are from the official paired evaluation unless marked local.

## Final result (submitted version: branch `ladder/it50bulk`, commit `eef08916b`)

| metric | value |
| --- | --- |
| build speedup vs frozen V5 baseline | **1.731x** |
| query speedup vs frozen V5 baseline | **1.781x** |
| hard gates | **pass** (Recall, VRAM, host memory, GPU path, query regression) |
| score | 12.99 |

## What was changed

1. **CAGRA graph build (the dominant cost).** The frozen baseline builds with
   NN-descent at 128 output / 256 intermediate degree and 100 iterations. The
   submission keeps the same fixed hipVS search implementation but builds the
   graph with 50 NN-descent iterations, 256 output / 384 intermediate degree,
   and the hash-pruned CAGRA graph optimizer. This is the smallest iteration
   count that still passes the Hidden250K Recall@100 baseline-delta gate: 45
   iterations already fails it, so the graph build sits exactly on the quality
   boundary.
2. **DDL snapshot append batching.** The offline DDL path fed the GPU index one
   row at a time (250,000 `add_snap_index` calls, each paying an adapter lookup,
   an arena reset and a buffer insert). Dense `lib=cuvs` indexes now hand a whole
   row-file batch to one call — the same call shape the async build path uses.
   Measured on the official evaluator with identical graph parameters:
   1.545x -> 1.731x.
3. **GPU search resource reuse** (earlier change): the cuVS search scratch
   buffers and search parameters are retained per index instead of being
   allocated, created and destroyed per query. Query speedup 1.689x -> 1.781x.

`SK_CUVS_NND_ITER`, `SK_CUVS_NND_DEGREE`, `SK_CUVS_NND_INTERNAL_DEGREE`,
`SK_CUVS_FINAL_DEGREE`, `SK_CUVS_SEARCH_ITOPK`, `SK_CUVS_SEARCH_WIDTH`,
`SK_CUVS_BULK_APPEND` keep the tuned values as defaults and let the benchmark
sweep A/B other settings without recompiling.

## Reproducible measurement method

`tools/bench/` measures the exact scored path on a GPU host:

```bash
tools/bench/run_contest_sweep.sh \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --work /work/sweep --memory 8G \
  --sift-base /work/sift_base.fbin --sift-query /work/sift_query.fbin \
  --configs "it50:50:256:384:128"
```

It imports 250K rows, times `CREATE VECTOR INDEX` from a cold index, times the
first fixed query (part of the build in the official rules), runs 4096
single-row Top-10/Top-100 queries, and reports QPS plus Recall against exact
ground truth. Phase timings are emitted to `$SK_CUVS_TIMING_FILE`
(`SEEKDB_GPU_BUILD`, `SEEKDB_BRIDGE_BUILD`, `SEEKDB_BRIDGE_SERIALIZE`,
`SEEKDB_CUVS_APPEND`, `SEEKDB_DDL_PHASE`).

## Local phase breakdown (real SIFT 250K x 128, gfx1100, 128 cores)

| phase | 100 iters (baseline shape) | 50 iters (submitted) |
| --- | ---: | ---: |
| NN-descent | ~38 s | 19.6 s |
| sort + prune + attach | ~2.5 s | 2.6 s |
| CAGRA serialize + snapshot LOB write | 0.5 s | 0.5 s |
| DDL scan + append + commit | 3.2 s | 3.2 s |
| **total build** | **43.9 s** | **25.9 s** |

The per-row append was 250,000 calls before the batching change; after it the
append phase itself is 0.34 s and the remaining DDL cost is the table scan and
the index commit.

NN-descent itself measures as ~7.5 s of fixed cost (buffer setup, random graph
initialisation and sampling) plus ~0.29 s per iteration at intermediate degree
384 on a 128-core host. At the recall-safe 50 iterations that is ~22 s of GPU
work. The Hidden250K Recall@100 baseline-delta gate fails at 45 iterations, so
the graph build cannot be made cheaper on this path without losing the gate:
that is the hard ceiling of this submission (1.731x).

## Negative results (measured, kept here so they are not retried)

| attempt | outcome |
| --- | --- |
| 45 / 40 / 30 / 20 NN-descent iterations | Hidden250K Recall@100 baseline delta fails |
| search beam `itopk` 768 / 1024 | query fails (10 s timeout, CPU fallback) |
| `search_width` 4 -> 16 with 30 iterations | queries 1.78x faster, Recall gap not recovered |
| parallel DDL (`_force_parallel_ddl_dop=8`) | build/write task races the append pipelines, index incomplete, Recall fails |
| cuVS `ITERATIVE_CAGRA_SEARCH` build | 49 s build (2x slower), Recall slightly lower |
| cuVS `IVF_PQ` build, defaults and explicit n_lists/n_probes/refinement | Recall 0.016 / 0.29, unusable |
| vendored GNND vs library NN-descent | identical build time |
| narrow NN-descent lists (int 128) with 60/80/100 iterations | cost is dominated by a ~7 s fixed part plus ~0.3 s per iteration, so a narrower list does not pay off |
| IVF-PQ candidate graph (`cuvsIvfPqBuild` + `cuvsIvfPqSearch` with a tuned `n_lists`/`n_probes`, then exact-distance sort + CAGRA optimize) | library requires int64 neighbours; with n=250K queries and k=degree the search sorts the candidate list per query and runs for minutes at 100% GPU, so it cannot beat 50 NN-descent iterations |
| subsample NN-descent + CAGRA-search graft of the remaining rows (`SK_CUVS_BUILD_ALGO=graft`, batch=2048, itopk=2*degree) | 1/4 subsample: build 29.8 s (vs 26.5 s for plain NN-descent) and Recall@100 0.924 (vs 0.9985). The all-query graft search costs as much as the NN-descent it replaces, and the grafted rows only see subsample neighbours, so both cost and quality get worse |
