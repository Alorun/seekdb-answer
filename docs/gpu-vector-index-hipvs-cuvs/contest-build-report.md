# seekdb × hipVS CAGRA build optimization — contest report

Task: accelerate `CREATE VECTOR INDEX ... lib=cuvs` (FP32, L2, CAGRA on AMD
gfx1100) end to end while keeping Recall, query throughput and resource gates.
All numbers below are from the official paired evaluation unless marked local.

## Final result (submitted version: branch `perf/bruteforce`, commit `4d9ca8929`)

| metric | value |
| --- | --- |
| build speedup vs frozen V5 baseline | **3.021x** |
| query speedup vs frozen V5 baseline | **1.765x** |
| hard gates | **pass** (Recall, VRAM, host memory, GPU path, query regression) |
| score | **20 / 20** |

Score breakdown: `4 (gates) + 14·clamp(ln 3.021/ln 3) + 2·clamp(ln 1.765/ln 1.5)`
= `4 + 14 + 2 = 20`.

## What was changed

1. **Exact-kNN GPU graph build (the dominant cost).** The frozen baseline runs
   GPU NN-descent (128 output / 256 intermediate degree, 100 iterations). The
   submission replaces it with an exact k-NN graph: the frozen hipVS
   `cuvsBruteForceBuild` / `cuvsBruteForceSearch` C API computes the top
   `degree+1` candidates per row on the GPU (FP16 candidate GEMM with FP32
   distances and norms), the bridge drops the self edge and re-sorts the
   candidates by exact distance, and the CAGRA graph optimizer prunes them to the
   output degree. The query path stays the frozen CAGRA search.
   - Candidates are computed at 128 degree, and `optimize` uses the
     exact-degree fast path (candidate degree == output degree) so the detour
     prune does no work; the graph keeps the full 128-degree quality.
   - Brute force is used for `n <= 400K` (both contest datasets), NN-descent
     otherwise; `SK_CUVS_BUILD_ALGO` still selects `brute`/`bf`, `nnd` or the
     diagnostic `floor` ring graph.
2. **DDL snapshot append batching.** The offline DDL fed the GPU index one row
   at a time (250,000 `add_snap_index` calls). Dense `lib=cuvs` indexes now hand
   a whole row-file batch to one call (982 calls for 250K rows). The bulk path
   also builds the per-row extra-object array, so indexes that carry the hidden
   pk / vid extra columns stay on the bulk path. Measured locally: SIFT build
   7961 -> 7661 ms.
3. **Parallel offline DDL.** The build pipeline was a sibling of the append
   pipelines and a single DDL slice made the table scan and row-file round trip
   serial. The DDL now defaults to 8 slices and the build pipeline waits until
   `get_snap_index_row_cnt() == row_cnt_` before serializing. The graph is
   order-independent (exact k-NN), so this is graph-neutral: Recall is unchanged
   and the DDL drops from 6.56 s to 2.81 s on the pure-DDL floor probe.
4. **GPU search resource reuse** (earlier change): the cuVS search scratch
   buffers and parameters are retained per index instead of being created and
   destroyed per query (query 1.689x -> 1.765x).

`SK_CUVS_BF_DEGREE`, `SK_CUVS_BF_CHUNK`, `SK_CUVS_BF_FP16`, `SK_CUVS_BULK_APPEND`
keep the tuned values as defaults and let the benchmark sweep A/B other settings
without recompiling.

## Reproducible measurement method

`tools/bench/` measures the exact scored path on a GPU host:

```bash
tools/bench/run_contest_sweep.sh \
  --observer build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --work /work/sweep --memory 8G \
  --sift-base /work/sift_base.fbin --sift-query /work/sift_query.fbin \
  --configs "sub:0:256:384:128"
```

It imports 250K rows, times `CREATE VECTOR INDEX` from a cold index, times the
first fixed query (part of the build in the official rules), runs 4096
single-row Top-10/Top-100 queries, and reports QPS plus Recall against exact
ground truth. Phase timings are emitted to `$SK_CUVS_TIMING_FILE`
(`SEEKDB_GPU_BUILD`, `SEEKDB_BF_BUILD`, `SEEKDB_BRIDGE_SERIALIZE`,
`SEEKDB_CUVS_APPEND`, `SEEKDB_DDL_BATCH`).

## Local phase breakdown (official GPU image, 24-core quota, 250K rows)

| phase | SIFT 128d | synthetic 256d |
| --- | ---: | ---: |
| DDL (parallel scan + bulk append + commit) | 2.2 s | 2.8 s |
| GPU build (`SEEKDB_GPU_BUILD`: BF + sort + prune + attach) | 4.10 s | 5.75 s |
| — of which brute-force candidate search | 3.51 s | 5.20 s |
| CAGRA serialize (snapshot) | 0.18 s | 0.25 s |
| snapshot stream write | 0.33 s | 0.52 s |
| first fixed query | 0.04 s | 0.04 s |
| **total build (`build_ms`)** | **6.61 s** | **8.82 s** |
| Recall@10 / @100 | 0.9990 / 0.9990 | 0.9978 / 0.9984 |

Projection against the documented baseline medians (23 s / 30 s):
`23/6.61 = 3.48x`, `30/8.82 = 3.40x`, `B = 3.44`. The official paired run
measured 3.021x, i.e. the evaluator host is ~1.1x slower than this 24-core
measurement, as expected.

## Negative results (measured, kept here so they are not retried)

| attempt | outcome |
| --- | --- |
| single-pass reverse-edge scatter (atomic arrival order) | graph order perturbation dropped Hidden250K Recall@100 below the baseline-delta gate -> submission scored 0 |
| `45 / 40 / 30 / 20` NN-descent iterations | Hidden250K Recall@100 baseline delta fails |
| search beam `itopk` 768 / 1024 | query fails (10 s timeout, CPU fallback) |
| `search_width` 4 -> 16 with 30 iterations | queries 1.78x faster, Recall gap not recovered |
| parallel DDL without a build/append dependency | build races the append pipelines, index incomplete, Recall fails (fixed by the row-count wait) |
| cuVS `ITERATIVE_CAGRA_SEARCH` build | 49 s build (2x slower), Recall slightly lower |
| cuVS `IVF_PQ` build, defaults and explicit n_lists/n_probes/refinement | Recall 0.016 / 0.29, unusable |
| vendored GNND vs library NN-descent | identical build time |
| IVF-PQ candidate graph (`cuvsIvfPqBuild` + `cuvsIvfPqSearch`, tuned `n_lists`/`n_probes`) | library requires int64 neighbours; the per-query sort runs for minutes at 100% GPU |
| subsample NN-descent + CAGRA-search graft | 1/4 subsample: build 29.8 s vs 26.5 s and Recall@100 0.924 vs 0.9985 |
| RAFT tiled half brute force (`raft::neighbors::detail::tiled_brute_force_knn`) | `raft/distance/distance.cuh` does not compile under clang 20 (ambiguous `__half operator*` in the correlation distance op) |
| hand-written tiled kNN (hipBLAS FP16 output + `raft::matrix::select_k`) | compiled and ran but the selected distances did not match the exact k-NN (self distance -0.97, ~12% neighbour distance error); 256d was 17% faster, 128d slower, so the path was dropped |
| `_force_parallel_ddl_dop=8` session override alone | same race as above; the dependency fix is required |
