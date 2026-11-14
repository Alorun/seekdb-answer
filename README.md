# MiniVectorDB

A small single-process vector database with a SQL subset, persistent WAL
storage, transactions and crash recovery. This is the competition entry for
`2026-competition-round2`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## Run

```bash
./build/minivectordb --data-dir /data
```

The default protocol is the official **JSON Lines** protocol: one JSON request
per line on stdin, one JSON response per line on stdout, and the startup
handshake is exactly:

```json
{"type":"ready"}
```

Then requests look like:

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'你好');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, content FROM docs ORDER BY id;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
```

Responses carry the same `id`; query responses include typed `columns` and
`rows`, DML responses include `affected_rows`, failures use
`{"ok":false,"error":{"code":"...","message":"..."}}`, and shutdown responds
`{"id":4,"ok":true}` then exits 0. The official `protocol_check/run.py` from the
task pack passes handshake and all 20 public cases against this image.

For local SQL-script tests only, `--protocol=text` enables the legacy
line/semicolon profile (`ready`, `OK <n>`, `ERROR ...`). It is not the
submission protocol.

## Supported SQL

- `CREATE TABLE` with `BIGINT`, `VARCHAR(n)`, `VECTOR(n)` and one BIGINT
  primary key.
- `DROP TABLE [IF EXISTS]`, `USE`, and common `SET` session statements
  (accepted as no-ops where the task does not need them). `TRUNCATE` and
  `CREATE TABLE IF NOT EXISTS` are outside the task subset and return
  `unsupported_sql`.
- `INSERT` (including multi-row).
- `SELECT` with column projection / `*`, `WHERE` comparisons and AND,
  `ORDER BY`, `LIMIT`, and `L2_DISTANCE`.
- `UPDATE` and `DELETE` with optional `WHERE`.
- `BEGIN` / `COMMIT` / `ROLLBACK`; autocommit when no transaction is active;
  `SET autocommit=0/1` follows MySQL (implicit transaction, commit on turn-on).
- `CREATE VECTOR INDEX` (catalog metadata; the vector scan itself is the
  exact q7 lower-bound path described below).

If the driver supplies `--ready-marker TOKEN`, the token is emitted after every
statement response (and not as a startup line when the first SQL is already
pending), so the driver can frame each reply. Without it, a single `ready` line
is printed at startup.

The competition's public cases send one complete SQL per line and assert
`affected_rows`, `columns` and `rows`. The default protocol is therefore
**line framing + an explicit `OK <affected>` status for DML/DDL** (and
`ERROR <code>: <message>` for failures). For local multi-line scripts use
`--statement-mode semicolon`; for quiet mysqltest-style output use
`--dml-ack=none`. Input is read in raw chunks, so a semicolon-terminated
statement does not need a trailing newline.

## Vector search fast path

Every exact TopK starts with one 7-bit scalar-quantized pass (one byte per
component, `max|v|/63` scale, AVX2 `maddubs`).  The pass keeps the code-space
k-th distance `d_k`; the triangle-inequality bound

    | ||v-q|| - scale*||code(v)-code(q)|| | <= err(v) + err(q)

proves that every true top-k row has a code-space distance at most
`(sqrt(d_k) + 2*(max_err+query_err)/scale)^2`.  Only that superset is
re-evaluated with the exact float32/double kernel, so `exact` returns exactly
the same ids, order and distances as a full scan while touching a few dozen
rows instead of re-ranking hundreds.  The superset candidates are prefetched
before the exact calls.

Equality-filtered TopK (`WHERE category = '...'`) also uses a contiguous q7
block per filter value: the table-wide code matrix is strided by the number of
distinct values, so the per-value copy turns the scan into a sequential stream
while keeping the same provable superset.  The blocks are rebuilt when the
table's row version changes (INSERT/UPDATE/DELETE/ROLLBACK).

`ann` mode has two data-gated paths.  Clustered tables use a q7 IVF candidate
index (nlist 64, nprobe 6, pool 24) that reuses the existing code matrix and
re-ranks its candidate pool with the exact float kernel; its first use runs an
8-query self-check against the exact radius-bounded IVF and falls back to that
exact walk if recall is below 0.98.  Uniform tables use the same q7 superset
scan as exact mode (IVF on uniform data has recall ~= nprobe/nlist and is not
used).  `MVDB_ANN_POOL`, `MVDB_ANN_NPROBE` and `MVDB_ANN_NLIST` tune the index
for experiments; defaults are 24/6/64.  Returned distances are always exact
float32 and ids are unique.

When two physical cores are available, the scan is split in half and run on a
persistent worker thread; each half computes its own superset and the exact
top-k results are merged.  On an SMT pair the pool falls back to one thread.

## Persistence

All state is written under `--data-dir` (default `/data`). The WAL uses
checksummed records and a commit marker; a commit acknowledges only after
`fdatasync`. CRC32C uses the SSE4.2 instruction with a startup self-test
against the portable table implementation. Multiple multi-row INSERTs that
arrive in one input burst append their commit markers first and share one
`fdatasync`, with all responses released only after that sync. The WAL is
preallocated with `fallocate(FALLOC_FL_KEEP_SIZE)` so the per-batch sync does
not journal a new extent. A single logical INSERT larger than the record
reader's cap is split into <=4 MiB / 4096-row WAL batches under one commit;
each row keeps the offset of its own record. On restart, only committed
transactions are replayed and an uncommitted tail is discarded.

Each table keeps a `primary key -> WAL offset` index. Small tables (default
50,000 rows) keep a row cache in memory; larger tables switch to a disk mode
and stream WAL records for scans (`--memory-rows N` to tune). On a clean
shutdown, a WAL larger than 64 MiB is compacted into a new WAL containing only
live rows, table definitions and index metadata (`--checkpoint-bytes N`).

## Known limitations

- No NULL, JOIN, aggregates, subqueries, ALTER TABLE, foreign keys, nested
  transactions or primary-key updates.
- DDL inside an explicit transaction is rejected.
- `ann` mode is approximate on clustered data (q7 IVF, recall validated 1.0
  on the clustered test set, 0.98 self-check fallback).  There is no graph
  index: measured HNSW/PQ prototypes were slower than q7 on uniform
  10k x 128, and IVF on uniform data has recall ~= nprobe/nlist, so no safe 3x
  win exists there.  Uniform ann uses the exact q7 superset path.
- There is no AVX-512 path: the platform A/B never proved it faster, and a
  bad ZMM/OS state previously invalidated a whole performance run.
- Disk-mode queries scan the WAL; immutable segments are a later milestone.
- An HNSW prototype is in `src/ann/` and `experiments/ann/`, but it is not
  linked into the server: at 10k×128 uniform data its recall-0.98 latency is
  only 1.5–2× better than exact, so exact remains the default. See
  `experiments/ann/NOTES.md`.
- The stdin/stdout profile is still a platform-dependent assumption; see
  `docs/PROTOCOL.md`.
