# Protocol profile (current assumptions)

The official test pack is not public. This file records exactly what the current
build assumes, so each submission can switch one variable at a time.

## Startup

- Recover the WAL and rebuild catalog/index metadata before printing anything.
- Print `ready\n` to stdout and flush.
- All diagnostics go to stderr.

Configurable with `--ready-marker`. An empty marker disables the line.
The same knobs can be set with `MVDB_READY_MARKER`, `MVDB_STATEMENT_MODE`,
`MVDB_DML_ACK`, and `MVDB_QUERY_MODE`; CLI arguments override them.

## Statement framing

- Input is read in raw byte chunks, not by lines; a statement does not need a
  trailing newline.
- Statements are normally terminated by `;` (outside strings/comments).
- A statement that is complete at end-of-line but has no `;` is held until the
  next chunk arrives; this keeps multi-line statements working.
- `--statement-mode=line` executes a complete statement at end-of-line (for a
  driver that does not send terminators). `semicolon` is the default. Line
  mode is one-line-only: continuation clauses on later lines are not supported;
  use semicolon mode for formatted multi-line SQL.
- At EOF, a final statement without `;` is executed.
- `--` (followed by whitespace), `#`, and `/* ... */` comments are skipped.
- Strings use single quotes; `''`, `\'`, `\\`, `\n`, `\t` are supported.

This means the driver can either send a whole script or one terminated
statement at a time. A driver that sends one statement per line **without** a
terminator and then waits would currently block; this is a known assumption to
verify with the platform.

## Results

`SELECT` prints a header line and then rows, fields separated by TAB:

```text
id	content	distance
1	事务与恢复	0.141421374
```

- A zero-row result prints only the header.
- Distances use `%.9g`.
- DML/DDL produce no stdout by default. `--dml-ack` prints `OK <affected>`;
  this is the main protocol variable to switch if the driver waits for a DML
  acknowledgement.
- This is the mysqltest-like assumption; `--protocol` is accepted but only the
  `mysqltest` profile is implemented so far.

## Errors

```text
ERROR <mysql-like-code>: <message>
```

Examples: 1064 syntax, 1050 table exists, 1146 table not found, 1054 column not
found, 1062 duplicate primary key, 1136 column count, 1406 data too long,
1366 invalid value, 1235 unsupported.

## Query mode

- Default `exact` (always exact scan).
- `--query-mode=exact|ann` accepted.
- `SET query_mode='ann';` / `SET @@query_mode='ann';` accepted in-session.
- `ann` currently falls back to exact execution; recall is 1.0.

## Session compatibility

`SET NAMES ...`, `SET autocommit=...`, `SET @@session...`, `USE db` and similar
session setup statements are accepted as no-ops (the task semantics remain:
autocommit plus explicit `BEGIN`). `CREATE TABLE IF NOT EXISTS`,
`DROP TABLE [IF EXISTS]` and `TRUNCATE [TABLE]` are supported. The test-only
`SET __mvdb_sync__='1'` prints `__mvdb_sync__` and is used by the local fuzz
harness; it is not part of the competition SQL contract.

## Exit

- EOF, `EXIT`, `QUIT`, SIGINT, SIGTERM: roll back an active transaction, sync
  anything needed, exit 0. SIGKILL: recovery on next start.
