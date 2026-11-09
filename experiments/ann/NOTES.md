# ANN prototype notes (HNSW)

All files here and in `src/ann/` are new. No tracked source, CMake, test, or doc
file was modified by this experiment. The prototype is single-threaded and
uses only C++20 + the standard library.

## What was built

`src/ann/ann_index.{h,cpp}` is a self-contained HNSW index:

* dense internal ids: row `i` passed to `build()` gets id `i`;
* squared-L2 distance, AVX2+FMA when compiled with `-mavx2 -mfma`, scalar
  fallback otherwise;
* standard HNSW construction descent, per-layer best-first search, diversity
  neighbour heuristic (with nearest-first filling), bidirectional links and
  pruning at `M0 = 2*M` (layer 0) / `M` (upper layers);
* deterministic level generation via SplitMix64 seeded by the constructor
  `seed`; ties are broken by id.  Same seed + same data => same graph/results;
* `search()` is `const` and uses per-call scratch, so concurrent queries after
  a build are safe.  `build()` itself is not concurrent-safe.

The suggested interface is preserved, with a seed and a few introspection
helpers added:

```cpp
mvdb::ann::AnnIndex index(dim, M, ef_construction, seed);
index.build(data);                          // data[i] -> internal id i
auto ids = index.search(q, k, ef_search);   // internal ids, nearest first
index.memoryBytes();                         // owned vectors + adjacency
```

## Benchmark

`bench_ann.cpp` generates 10k x 128 float32 vectors and 500 queries.  Truth is
brute-force squared L2 + top-10 (`partial_sort`).  Datasets:

* uniform `[-1,1]^128`, seeds 42/43/44;
* clustered: 20 uniform centers, uniform `[-0.35,0.35]` offsets, seed 42.

Build/run from the repo root:

```bash
make -C experiments/ann clean
make -C experiments/ann run
# Optional tuned configuration (separate binary):
make -C experiments/ann M=32 EFC=200 run
```

The default binary is `build-ann/ann_bench_m16_efc200`; outputs are saved in
`experiments/ann/benchmark_m16.txt` and `benchmark_m32_efc200.txt`.
Timings are machine-dependent; recall/build values for a fixed seed are stable
(verified by repeated runs).  Exact AVX2 is ~0.42 ms/query on this machine
(measurement from the parent task; 10k x 128, top-10), used below only as a
reference.

### M=16, efConstruction=200 (default)

Memory 6.73 MiB (index-owned vectors + graph); build 1.58 s clustered and
3.07-3.38 s uniform.

| dataset/seed | ef=16 | ef=32 | ef=64 | ef=128 | ef=256 |
|---|---:|---:|---:|---:|---:|
| uniform/42 recall | .4192 | .6016 | .7836 | .9140 | .9806 |
| uniform/42 p50/p99 us | 22.6/40.8 | 43.8/76.7 | 80.1/159.6 | 150.3/244.2 | 280.6/464.0 |
| uniform/43 recall | .4242 | .6086 | .7902 | .9220 | .9830 |
| uniform/44 recall | .4272 | .6000 | .7834 | .9184 | .9810 |
| clustered/42 recall | .9248 | .9878 | .9990 | 1.0000 | 1.0000 |
| clustered/42 p50/p99 us | 16.1/36.2 | 24.8/50.4 | 40.3/79.3 | 77.7/147.9 | 106.7/188.2 |

Uniform recall at ef=256 is only .981-.983, with p50 261-286 us.  This is a
~1.5-1.6x speedup over exact, not a compelling default.  Clustered data is
much easier: .988 at ef=32 and 25 us, i.e. roughly 17x exact.

### M=32, efConstruction=200 (tuned for higher recall)

Memory 7.94 MiB; build 4.37 s clustered and 10.46-11.04 s uniform.

| dataset/seed | ef=64 | ef=128 | ef=256 |
|---|---:|---:|---:|
| uniform/42 recall | .9248 | .9860 | .9990 |
| uniform/42 p50/p99 us | 129.6/389.7 | 221.9/310.3 | 381.1/683.7 |
| uniform/43 recall | .9296 | .9902 | .9996 |
| uniform/44 recall | .9306 | .9872 | .9986 |
| clustered/42 recall | 1.0000 | 1.0000 | 1.0000 |
| clustered/42 p50/p99 us | 38.2/59.2 | 54.6/86.2 | 85.6/130.9 |

At ef=128 all three uniform seeds are >=.986, but p50 is ~201-222 us and the
build is >10 s for only 10k vectors.  Uniform data in 128 dimensions is close
to a worst case for graph ANN (neighbour relations are noisy); the clustered
case is more representative of embedding-like data.

## Recommendation

**Do not enable HNSW by default for uniform 10k x 128 yet; keep exact.**  At
that scale exact SIMD is ~0.42 ms/query, while HNSW needs ~260 us (M=16,
ef=256, recall .981) or ~210 us (M=32, ef=128, recall .986), so the gain is
only 1.5-2x and it adds approximation, 6.7-7.9 MiB, a 3-11 s build, and
mutation/consistency work.

**Do integrate it as an opt-in `ann` mode when one of these becomes true:**

* the table grows well beyond 10k (exact is O(N); graph search is roughly
  logarithmic), or
* vectors are clustered / embedding-like, where M=16 + ef=32 already gives
  .988 at ~25 us, or
* a strict latency bound cannot be met by exact search.

If a mode must ship now, use M=32, efConstruction=200, efSearch=128 (recall
.986-.990, p50 ~200-222 us) with exact fallback when recall/ef validation
fails.  M=16/ef=256 is the cheaper-build alternative (3 s, .981).

IVF was not implemented.  It would train k-means centroids and tune `nprobe`;
expect cheaper build/lower memory but generally weaker high-recall latency than
HNSW here.  HNSW also maps more naturally to incremental inserts and tombstone
deletes.  The HNSW fallback risk is build cost, not query quality, on this
dataset.

## Integration notes (future `ann` query mode)

### Dense internal id -> table PK

The graph stores only `uint32_t` dense ids assigned by row order.  Keep a
side table `internal_id -> RowLocator` (`RowLocator` should be the stable
physical row locator used by the row store, not necessarily the SQL PK).
`search()` returns internal ids; translate through that vector and then through
the row store's PK mapping.  Rebuild the side table in lockstep with the graph.
Keep a reverse map if updates/deletes need to find an internal id.

### Inserts, updates, deletes

The prototype is **bulk-build only**.  HNSW can append incrementally (the
private `insert` path exists), but exposing it needs transactional handling:

* **Insert**: build the index from a committed snapshot, or add an explicit
  append API that allocates a new internal id and maps it to the new locator.
  Do not reorder existing internal ids, or the graph becomes invalid.
* **Update**: HNSW links are based on the old vector.  Tombstone the old id and
  append the new version (or rebuild); never mutate the vector in place.
* **Delete**: physically repairing a graph is expensive.  Use a tombstone /
  visibility check in the executor, over-fetch (`ef` and/or `k` x 2-4), and
  filter dead rows.  Rebuild when tombstones exceed ~10-20%.
* **MVCC/recovery**: the index must be derived from the committed row store,
  not from WAL replay directly.  Index entries may be stale for a snapshot;
  the executor must verify visibility/version after id translation.  If that
  check is too expensive, index only the latest committed snapshot and rebuild
  after recovery/compaction.
* If the table is small or churn is high, route to exact search; this is the
  safest fallback.

### Filtered queries

* **Small selectivity** (roughly <1-5%, or a few thousand candidate rows):
  pre-filter the candidate locators and run an exact SIMD scan.  HNSW
  post-filtering has poor recall for selective predicates.
* **Large selectivity**: over-fetch from HNSW (e.g. `ef_search * 2-4`,
  `k * 4`) then apply the predicate and top-k merge.  Better: add a
  filter-aware traversal that still walks filtered nodes but does not add them
  to results.  The current API has no predicate callback.
* If a predicate is very common (tenant/category), consider a separate index
  per partition.

### Remaining work before integration

1. Add append/delete/visibility hooks and a snapshot/rebuild policy.
2. Add graph serialization or accept rebuild-on-startup (build is 1.6-11 s for
   10k; that may be acceptable asynchronously, not on the query path).
3. Avoid copying the table twice: the prototype owns a dense copy; integration
   could borrow/move a vector snapshot or use a compressed representation.
4. Choose M/ef against the real corpus, not uniform random data; log recall
   against exact for a sample and fall back when below target.
5. Concurrency: search is read-safe after build, but build and graph mutation
   need a lock or copy-on-write snapshot.
6. Add integration tests for id mapping, deletes, filtered queries, rollback,
   and crash recovery; benchmark P99 under concurrent load.

## Risks / limitations

* Uniform random 128-d is a known hard case; results may understate or
  overstate performance on real embeddings.  Re-benchmark on production-like
  vectors before enabling.
* Approximate recall is data-dependent; a fixed `ef_search` cannot guarantee
  .98 on all tables.
* Build is single-threaded and costs 3-11 s at 10k (much more with larger M);
  it must not block foreground queries.
* No deletion compaction; tombstone buildup degrades recall/latency.
* `memoryBytes()` excludes allocator metadata, so RSS is somewhat higher
  (observed roughly 6.7-7.9 MiB of owned index data).
* Internal ids are `uint32_t` (4.29B max); guard table size if this prototype
  is used beyond the competition scale.
