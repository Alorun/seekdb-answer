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

- `CREATE TABLE` / `CREATE TABLE IF NOT EXISTS` with `BIGINT`, `VARCHAR(n)`,
  `VECTOR(n)` and one BIGINT primary key.
- `DROP TABLE [IF EXISTS]`, `TRUNCATE [TABLE]`, `USE`, and common `SET`
  session statements (accepted as no-ops where the task does not need them).
- `INSERT` (including multi-row).
- `SELECT` with column projection / `*`, `WHERE` comparisons and AND,
  `ORDER BY`, `LIMIT`, and `L2_DISTANCE`.
- `UPDATE` and `DELETE` with optional `WHERE`.
- `BEGIN` / `COMMIT` / `ROLLBACK`; autocommit when no transaction is active.
- `CREATE VECTOR INDEX` (catalog acceptance; exact execution fallback).

The competition's public cases send one complete SQL per line and assert
`affected_rows`, `columns` and `rows`. The default protocol is therefore
**line framing + an explicit `OK <affected>` status for DML/DDL** (and
`ERROR <code>: <message>` for failures). For local multi-line scripts use
`--statement-mode semicolon`; for quiet mysqltest-style output use
`--dml-ack=none`. Input is read in raw chunks, so a semicolon-terminated
statement does not need a trailing newline.

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
- Disk-mode queries scan the WAL; immutable segments are a later milestone.
- An HNSW prototype is in `src/ann/` and `experiments/ann/`, but it is not
  linked into the server: at 10k×128 uniform data its recall-0.98 latency is
  only 1.5–2× better than exact, so exact remains the default. See
  `experiments/ann/NOTES.md`.
- The stdin/stdout profile is still a platform-dependent assumption; see
  `docs/PROTOCOL.md`.
