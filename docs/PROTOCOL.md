# MiniVectorDB protocol profile

The competition protocol is the official **JSON Lines** stdin/stdout protocol
from `STDIO_PROTOCOL.md` in the round-2 pack. The database defaults to it.

## Startup

After data recovery, print exactly one line and flush before reading stdin:

```json
{"type":"ready"}
```

No `READY`, prompt, banner, or extra field is written to stdout. Diagnostics go
to stderr. Every restart (normal or recovery) prints ready again.

## Requests

One request per physical line, UTF-8 and newline-terminated:

```json
{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}
{"id":2,"op":"shutdown"}
```

`id` is echoed with the same JSON type; `op` is `execute` or `shutdown`;
`search_mode` is `exact` or `ann` (ANN currently uses the exact fallback, so
recall is 1.0).

## Responses

Success, query:

```json
{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}
```

Success, DML:

```json
{"id":2,"ok":true,"affected_rows":1}
```

DDL, transaction control and SET return `{"id":N,"ok":true}` with no
`affected_rows`. Failure:

```json
{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
```

`ok` is a JSON boolean; `error.code` is a non-empty semantic string. Expected
SQL errors do not terminate the process. BIGINT is a JSON integer (no float
round-trip); strings are JSON strings; vector projections are JSON arrays of
float32 values; rows are two-dimensional arrays.

## Shutdown

`{"id":N,"op":"shutdown"}` rolls back an active transaction, checkpoints if
needed, responds `{"id":N,"ok":true}`, flushes, and exits 0. No bytes are
written after the shutdown response.

## Legacy text profile

`--protocol=text` is for local SQL-script tests only. It keeps the previous
`ready` line, line/semicolon SQL framing, `OK <n>` status, `ERROR ...` lines,
and the test-only `--ready-marker` behavior. It is not used by the judge.

## SQL compatibility

- `CREATE TABLE` with `BIGINT`, `VARCHAR(n)`, `VECTOR(n)`, one BIGINT primary key.
- `DROP TABLE [IF EXISTS]`, `USE`, common `SET` no-ops.
- `TRUNCATE` and `CREATE TABLE IF NOT EXISTS` are outside the task subset and
  return `unsupported_sql`.
- `INSERT` (multi-row and explicit column lists), `SELECT` projection/`*`,
  `WHERE` comparisons with AND, `ORDER BY` (column, alias, ordinal, or
  `L2_DISTANCE` expression), `LIMIT`, `L2_DISTANCE`.
- `UPDATE`, `DELETE`, `BEGIN`/`COMMIT`/`ROLLBACK`, autocommit.
- `CREATE VECTOR INDEX` catalog metadata; exact execution fallback.

## Persistence

All state is under `--data-dir` (default `/data`). WAL records are checksummed
and have a commit marker; commit acknowledges only after `fdatasync`. Recovery
replays committed transactions only and truncates an uncommitted/invalid tail.
Small tables (default 50,000 rows and 64 MiB estimated) cache rows in memory;
larger/wide tables stream WAL records. Clean shutdown compacts a WAL larger than
64 MiB (`--checkpoint-bytes`), and the WAL descriptor is reopened after the
atomic rename.

## Error codes

Semantic names are used in JSON `error.code`: `syntax_error`,
`duplicate_table`, `table_not_found`, `column_not_found`, `duplicate_key`,
`column_count_mismatch`, `value_too_long`, `dimension_mismatch`,
`invalid_vector`, `invalid_value`, `value_out_of_range`, `type_mismatch`,
`unsupported_sql`, `transaction_state`, `duplicate_index`, `index_not_found`,
`io_error`. The local text suites can request numeric MySQL-style codes with
`--numeric-errors`; the official JSON profile always uses semantic names.
