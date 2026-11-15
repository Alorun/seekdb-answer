# Running the performance benchmark in ann mode

`exact` remains the image default.  Do **not** change the default: the
functional MySQL cases may omit `search_mode` and must keep exact semantics.
The performance benchmark must select ann explicitly, by one of:

1. the submission form's query-mode field (`ann`), if present; or
2. the container environment `MVDB_QUERY_MODE=ann`; or
3. the process argument `--query-mode ann` (the Docker `CMD` can be extended by
   the platform only if the submission form supports extra args).

The program already honours all three.  Per-request `search_mode` always
overrides the default, so a mixed functional run keeps exact cases exact.

What ann does by data distribution:

- **uniform**: no approximate index is built; ann uses the same exact q7 scan
  as exact mode (recall 1.0, no speed gain, no memory cost).
- **clustered**: q7 IVF candidate index (nlist 64, nprobe 6, pool 24), then
  exact float re-rank.  A 16-sample self-check against exact IVF runs once per
  table; below 0.98 overlap it disables the index and falls back to exact IVF.
  Recall is 1.0 on the local clustered test before/after UPDATE+DELETE/restart.
  Local 10k x 128 clustered wall, 5 interleaved runs: exact 79.4 ms/1000,
  ann 59.3 ms/1000 (-25.3%).  Index bytes are small (tens of KB); build happens
  during index preparation/warmup, not in the timed query sequence.

Risk: if the benchmark data is uniform, ann gives no gain (it is exact); if the
platform ignores the ann config, E stays at the exact level.  The 5100 target
needs an additional M/B gain or a server-dominated query wall; do not assume it
from ann alone.
