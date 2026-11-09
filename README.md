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

The process reads SQL from stdin and writes results to stdout. Startup
diagnostics and errors are written to stderr. The default data directory is
`/data`.

```
ready
CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR(4));
INSERT INTO docs VALUES (1,'database','事务与恢复','[1,0,0,0]');
SELECT id, content FROM docs ORDER BY id;
```

## Supported SQL (v0.1)

- `CREATE TABLE` with `BIGINT`, `VARCHAR(n)`, `VECTOR(n)` and one BIGINT
  primary key.
- `INSERT` (including multi-row).
- `SELECT` with column projection / `*`, `WHERE` comparisons and AND,
  `ORDER BY`, `LIMIT`, and `L2_DISTANCE`.
- `UPDATE` and `DELETE` with optional `WHERE`.
- `BEGIN` / `COMMIT` / `ROLLBACK`; autocommit when no transaction is active.
- `CREATE VECTOR INDEX` (catalog acceptance; exact execution fallback).

## Persistence

All state is written under `--data-dir` (default `/data`). The WAL uses
checksummed records and a commit marker; a commit acknowledges only after
`fdatasync`. On restart, only committed transactions are replayed and an
uncommitted tail is discarded.

Each table keeps a `primary key -> WAL offset` index. Small tables (default
50,000 rows) keep a row cache in memory; larger tables switch to a disk mode
and stream WAL records for scans (`--memory-rows N` to tune). On a clean
shutdown, a WAL larger than 64 MiB is compacted into a new WAL containing only
live rows, table definitions and index metadata (`--checkpoint-bytes N`).

## Known limitations (v0.2)

- No NULL, JOIN, aggregates, subqueries, ALTER TABLE, foreign keys, nested
  transactions or primary-key updates.
- DDL inside an explicit transaction is rejected.
- Approximate index execution is not implemented yet; `ann` mode falls back to
  exact scan and therefore has recall 1.0 but exact-scan latency.
- Disk-mode queries scan the WAL; immutable segments and a real ANN index are
  later milestones.
- The stdin/stdout profile is still a platform-dependent assumption; see
  `docs/PROTOCOL.md`.
