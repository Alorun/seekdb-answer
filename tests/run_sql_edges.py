#!/usr/bin/env python3
"""SQL edge-case suite for the MiniVectorDB binaries.

Runs each case against the *existing* binary in two storage modes:

    * default (in-memory rows, spills at --memory-rows)
    * --memory-rows 0 (disk/WAL-scan mode)

The suite deliberately drives one statement at a time (write + read) so that
a parser error in one statement cannot hide later statements and so every
failure has a minimal SQL repro.  Distances are checked against a small
brute-force Python oracle (float32 inputs, tolerance on the float column).

Typical use::

    python3 tests/run_sql_edges.py
    python3 tests/run_sql_edges.py --bin ./build/minivectordb --bin ./build-lead/minivectordb
    python3 tests/run_sql_edges.py --mode memory      # only default mode
    python3 tests/run_sql_edges.py --mode disk        # only --memory-rows 0

Exit status is 1 when at least one check fails.
"""

from __future__ import annotations

import argparse
import contextlib
import math
import os
import random
import re
import struct
import sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))
from mvdb_harness import (Session, new_data_dir, remove_data_dir,  # noqa: E402
                          run_batch, strip_ready)

INT64_MIN = -9223372036854775808
INT64_MAX = 9223372036854775807


def f32(x: float) -> float:
    return struct.unpack("f", struct.pack("f", x))[0]


def fmt_vec(vec) -> str:
    return "[" + ",".join("%.6f" % v for v in vec) + "]"


class Failure:
    def __init__(self, case, sql, expected, observed):
        self.case = case
        self.sql = sql
        self.expected = expected
        self.observed = observed


class Runner:
    """One (binary, storage-mode) combination."""

    def __init__(self, bin_path: str, mode_args, mode_label: str):
        self.bin = bin_path
        self.mode_args = list(mode_args)
        self.mode_label = mode_label
        self.failures = []
        self.checks = 0
        self._sessions = []
        self._dirs = []
        self._case = "?"

    # ------------------------------------------------------------ lifecycle
    @contextlib.contextmanager
    def case(self, name):
        self._case = name
        d = new_data_dir("mvdb-sql-")
        self._dirs.append(d)
        try:
            yield d
        finally:
            for s in self._sessions:
                try:
                    s.close()
                except Exception:
                    pass
            self._sessions = []
            remove_data_dir(d)

    def session(self, data_dir):
        s = Session(self.bin, data_dir,
                    args=list(self.mode_args) + ["--dml-ack=none"])
        self._sessions.append(s)
        if not s.wait_lines(1, 2.0):
            self._record("startup", "<start>", "ready marker", s.out_text())
        s.pump(0.05)
        s.take_out()  # drop the ready marker
        return s

    def restart(self, s, data_dir):
        s.close()
        return self.session(data_dir)

    # -------------------------------------------------------------- checks
    def _record(self, name, sql, expected, observed):
        self.failures.append(Failure(name, sql, expected, observed))

    def silent(self, s, sql):
        self.checks += 1
        s.send(sql + "\n")
        s.pump(0.07)
        got = s.take_out()
        if got.strip():
            self._record(self._case, sql, "no stdout for DML/DDL",
                         got.splitlines())

    def error(self, s, sql, code, contains=None):
        self.checks += 1
        s.send(sql + "\n")
        s.wait_lines(1, 1.2)
        got = s.take_out().splitlines()
        want = "ERROR %d: %s" % (code, "<message>" if not contains else "*%s*" % contains)
        ok = len(got) == 1 and got[0].startswith("ERROR %d: " % code)
        if ok and contains and contains not in got[0]:
            ok = False
        if not ok:
            self._record(self._case, sql, want, got)
        return ok

    def rows(self, s, sql, header, rows, float_cols=(), tol=2e-4):
        self.checks += 1
        s.send(sql + "\n")
        s.wait_lines(1 + len(rows), 1.5)
        got = s.take_out().splitlines()
        want_desc = {"header": header, "rows": rows}
        if not got:
            self._record(self._case, sql, want_desc, got)
            return
        problems = []
        if got[0].split("\t") != header:
            problems.append("header %r != %r" % (got[0].split("\t"), header))
        body = got[1:]
        if len(body) != len(rows):
            problems.append("row count %d != %d" % (len(body), len(rows)))
        for i, (line, exp) in enumerate(zip(body, rows)):
            fields = line.split("\t")
            if len(fields) != len(exp):
                problems.append("row %d field count %d != %d (%r)"
                                % (i, len(fields), len(exp), fields))
                continue
            for j, (gv, ev) in enumerate(zip(fields, exp)):
                if j in float_cols:
                    try:
                        gf = float(gv)
                        ef = float(ev)
                    except ValueError:
                        problems.append("row %d col %d not float: %r vs %r"
                                        % (i, j, gv, ev))
                        continue
                    diff = abs(gf - ef)
                    if diff > tol * max(1.0, abs(ef)):
                        problems.append("row %d col %d float %r != %r (tol %g)"
                                        % (i, j, gv, ev, tol))
                elif gv != str(ev):
                    problems.append("row %d col %d %r != %r" % (i, j, gv, ev))
        if problems:
            self._record(self._case, sql, want_desc, problems or body)

    def header_only(self, s, sql, header):
        self.rows(s, sql, header, [])


# ---------------------------------------------------------------------------
# cases
# ---------------------------------------------------------------------------
def case_bigint_limits(r):
    with r.case("bigint_limits") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY);")
        r.silent(s, "INSERT INTO t VALUES (-9223372036854775808),"
                    "(9223372036854775807),(0),(-1),(1);")
        r.rows(s, "SELECT id FROM t ORDER BY id;", ["id"],
               [[str(INT64_MIN)], ["-1"], ["0"], ["1"], [str(INT64_MAX)]])
        r.rows(s, "SELECT id FROM t WHERE id = 9223372036854775807;",
               ["id"], [[str(INT64_MAX)]])
        r.rows(s, "SELECT id FROM t WHERE id = -9223372036854775808;",
               ["id"], [[str(INT64_MIN)]])


def case_bigint_overflow(r):
    with r.case("bigint_overflow") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY);")
        r.error(s, "INSERT INTO t VALUES (9223372036854775808);", 1366)
        r.error(s, "INSERT INTO t VALUES (-9223372036854775809);", 1366)
        r.error(s, "INSERT INTO t VALUES (1.5);", 1366)
        r.error(s, "INSERT INTO t VALUES ('1');", 1366)
        r.rows(s, "SELECT id FROM t;", ["id"], [])


def case_varchar_codepoints(r):
    with r.case("varchar_codepoints") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'你好');")
        r.silent(s, "INSERT INTO t VALUES (2,'😀😀');")
        r.error(s, "INSERT INTO t VALUES (3,'😀😀a');", 1406,
                contains="too long")
        r.silent(s, "INSERT INTO t VALUES (4,'');")
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "你好"], ["2", "😀😀"], ["4", ""]])
        r.rows(s, "SELECT id FROM t WHERE s = '';", ["id"], [["4"]])


def case_vector_validation(r):
    with r.case("vector_validation") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(3));")
        r.silent(s, "INSERT INTO t VALUES (1,'[1,2,3]');")
        r.error(s, "INSERT INTO t VALUES (2,'[1,2]');", 1366,
                contains="dimension mismatch")
        r.error(s, "INSERT INTO t VALUES (3,'[nan,0,0]');", 1366)
        r.error(s, "INSERT INTO t VALUES (4,'[inf,0,0]');", 1366)
        r.error(s, "INSERT INTO t VALUES (5,'[1e39,0,0]');", 1366)
        r.error(s, "INSERT INTO t VALUES (6,'[1,2,]');", 1366,
                contains="trailing")
        r.error(s, "INSERT INTO t VALUES (7,'[]');", 1366,
                contains="dimension mismatch")
        r.silent(s, "INSERT INTO t VALUES (8,'[ 1 , 2 , 3 ]');")
        r.rows(s, "SELECT id,v FROM t ORDER BY id;", ["id", "v"],
               [["1", "[1,2,3]"], ["8", "[1,2,3]"]])


def case_duplicate_pk(r):
    with r.case("duplicate_pk") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4));")
        r.silent(s, "INSERT INTO t VALUES (1,'a');")
        r.error(s, "INSERT INTO t VALUES (1,'b');", 1062,
                contains="duplicate primary key")
        # duplicate inside one multi-row INSERT: whole statement must be atomic
        r.error(s, "INSERT INTO t VALUES (2,'b'),(3,'c'),(2,'dup');", 1062)
        r.rows(s, "SELECT id FROM t ORDER BY id;", ["id"], [["1"]])


def case_unknown_objects(r):
    with r.case("unknown_objects") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8),"
                    " v VECTOR(2));")
        r.error(s, "SELECT id FROM nope;", 1146)
        r.error(s, "SELECT nope FROM t;", 1054)
        r.error(s, "SELECT id FROM t ORDER BY nope;", 1054)
        r.error(s, "INSERT INTO nope VALUES (1);", 1146)
        r.error(s, "INSERT INTO t (id,nope) VALUES (1,'x');", 1054)
        r.error(s, "UPDATE t SET nope='x';", 1054)
        r.error(s, "DELETE FROM nope;", 1146)


def case_unsupported(r):
    with r.case("unsupported") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'[1,0]');")
        # DROP/TRUNCATE/USE became supported in the newer lead build, so this
        # case pins statements that are still unsupported in both builds.
        r.error(s, "GRANT SELECT ON t TO u;", 1235)
        r.error(s, "SHOW TABLES;", 1235)
        r.error(s, "ALTER TABLE t ADD COLUMN x BIGINT;", 1235)
        r.error(s, "CREATE VIEW v AS SELECT 1;", 1064)
        r.error(s, "SELECT COUNT(*) FROM t;", 1064)
        r.error(s, "SELECT * FROM t JOIN t;", 1064)
        r.error(s, "UPDATE t SET id = 2;", 1235)
        r.error(s, "SELECT id FROM t WHERE v = '[1,0]';", 1235)


def case_ddl_edges(r):
    with r.case("ddl_edges") as d:
        s = r.session(d)
        r.error(s, "CREATE TABLE twovec (id BIGINT PRIMARY KEY,"
                   " a VECTOR(2), b VECTOR(2));", 1235)
        r.error(s, "CREATE TABLE nopk (id BIGINT, s VARCHAR(2));", 1064)
        r.error(s, "CREATE TABLE v0 (id BIGINT PRIMARY KEY, v VECTOR(0));",
                1366)
        r.error(s, "CREATE TABLE c0 (id BIGINT PRIMARY KEY, s VARCHAR(0));",
                1366)
        r.error(s, "CREATE TABLE cneg (id BIGINT PRIMARY KEY, s VARCHAR(-1));",
                1366)
        r.error(s, "CREATE TABLE vneg (id BIGINT PRIMARY KEY, v VECTOR(-1));",
                1366)
        r.error(s, "CREATE TABLE pkstr (s VARCHAR(2) PRIMARY KEY);", 1235)
        r.silent(s, "CREATE TABLE ok (id BIGINT PRIMARY KEY, s VARCHAR(2));")
        r.error(s, "CREATE TABLE ok (id BIGINT PRIMARY KEY);", 1050)


def case_insert_column_count(r):
    with r.case("insert_column_count") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4),"
                    " v VECTOR(2));")
        r.error(s, "INSERT INTO t VALUES (1,'a');", 1136)
        r.error(s, "INSERT INTO t VALUES (1,'a','[1,0]','extra');", 1136)
        r.error(s, "INSERT INTO t (id,s) VALUES (1,'a');", 1136)
        r.silent(s, "INSERT INTO t (v,s,id) VALUES ('[1,0]','a',1);")
        r.rows(s, "SELECT id,s FROM t;", ["id", "s"], [["1", "a"]])


def case_limit_zero(r):
    with r.case("limit_zero") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'[1,0]'),(2,'[0,1]');")
        r.header_only(s, "SELECT id FROM t LIMIT 0;", ["id"])
        r.header_only(s, "SELECT id FROM t ORDER BY id LIMIT 0;", ["id"])
        r.header_only(
            s, "SELECT id, L2_DISTANCE(v,'[1,0]') AS d FROM t"
               " ORDER BY d LIMIT 0;", ["id", "d"])


def case_order_strings(r):
    with r.case("order_strings") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));")
        r.silent(s, "INSERT INTO t VALUES (3,'b'),(1,'a'),(2,'a'),"
                    "(5,'A'),(4,''),(6,'中'),(7,'😀');")
        r.rows(s, "SELECT id,s FROM t ORDER BY s;", ["id", "s"],
               [["4", ""], ["5", "A"], ["1", "a"], ["2", "a"], ["3", "b"],
                ["6", "中"], ["7", "😀"]])
        # ties are broken by primary key ascending, including DESC
        r.rows(s, "SELECT id,s FROM t ORDER BY s DESC;", ["id", "s"],
               [["7", "😀"], ["6", "中"], ["3", "b"], ["1", "a"], ["2", "a"],
                ["5", "A"], ["4", ""]])
        r.rows(s, "SELECT id FROM t ORDER BY s LIMIT 3;", ["id"],
               [["4"], ["5"], ["1"]])
        r.rows(s, "SELECT id FROM t ORDER BY s DESC LIMIT 3;", ["id"],
               [["7"], ["6"], ["3"]])


def case_order_numeric_precision(r):
    """BIGINT ordering above 2^53 must not go through double.

    Expected numerically: ...992 (id 2) before ...993 (id 1).  The current
    executor compares ORDER BY keys as double, so both collapse to the same
    value and the PK tie-break emits 1 before 2 -> wrong order.
    """
    with r.case("order_numeric_precision") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, k BIGINT);")
        r.silent(s, "INSERT INTO t VALUES (1,9007199254740993),"
                    "(2,9007199254740992),(3,3),"
                    "(4,-9223372036854775808),(5,9223372036854775807);")
        r.rows(s, "SELECT id,k FROM t ORDER BY k;", ["id", "k"],
               [["4", str(INT64_MIN)], ["3", "3"], ["2", "9007199254740992"],
                ["1", "9007199254740993"], ["5", str(INT64_MAX)]])
        r.rows(s, "SELECT id FROM t ORDER BY k LIMIT 3;", ["id"],
               [["4"], ["3"], ["2"]])
        r.rows(s, "SELECT id FROM t ORDER BY k DESC LIMIT 3;", ["id"],
               [["5"], ["1"], ["2"]])
        r.rows(s, "SELECT id FROM t WHERE k = 9007199254740993;", ["id"],
               [["1"]])


def case_tie_break(r):
    with r.case("tie_break") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4),"
                    " v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (3,'a','[1,0]'),"
                    "(1,'a','[1,0]'),(2,'a','[1,0]');")
        r.rows(s, "SELECT id FROM t ORDER BY s LIMIT 2;", ["id"],
               [["1"], ["2"]])
        # DESC tie order: implementation keeps PK ascending (SQL leaves it
        # unspecified; recorded here so a change is visible).
        r.rows(s, "SELECT id FROM t ORDER BY s DESC LIMIT 2;", ["id"],
               [["1"], ["2"]])
        r.rows(s, "SELECT id, L2_DISTANCE(v,'[1,0]') AS d FROM t"
                  " ORDER BY d LIMIT 2;", ["id", "d"],
               [["1", 0.0], ["2", 0.0]], float_cols=[1])


def case_topk_oracle(r):
    rng = random.Random(20261109)
    dim = 6
    rows = []
    for pk in range(1, 61):
        cat = "c%d" % (pk % 5)
        vec = [f32(float("%.6f" % rng.uniform(-1, 1))) for _ in range(dim)]
        rows.append((pk, cat, vec))
    query_text = fmt_vec([rng.uniform(-1, 1) for _ in range(dim)])
    query = [f32(float("%.6f" % x)) for x in
             [float(t) for t in query_text.strip("[]").split(",")]]

    def dist(vec):
        return math.sqrt(sum((a - b) ** 2 for a, b in zip(vec, query)))

    # brute-force oracle for the expected rows
    def expected(cat=None, k=5, desc=False):
        cand = [(pk, dist(vec)) for pk, c, vec in rows
                if cat is None or c == cat]
        cand.sort(key=lambda t: (-t[1], t[0]) if desc else (t[1], t[0]))
        return [[str(pk), d] for pk, d in cand[:k]]

    with r.case("topk_oracle") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE topk (id BIGINT PRIMARY KEY,"
                    " category VARCHAR(8), v VECTOR(6));")
        for start in range(0, len(rows), 20):
            batch = rows[start:start + 20]
            values = ",".join("(%d,'%s','%s')" % (pk, cat, fmt_vec(vec))
                              for pk, cat, vec in batch)
            r.silent(s, "INSERT INTO topk VALUES " + values + ";")
        r.rows(s, "SELECT id, L2_DISTANCE(v,'%s') AS d FROM topk"
                  " ORDER BY d LIMIT 8;" % query_text,
               ["id", "d"], expected(k=8), float_cols=[1],
               tol=5e-4)
        r.rows(s, "SELECT id, L2_DISTANCE(v,'%s') AS d FROM topk"
                  " WHERE category = 'c3' ORDER BY d LIMIT 5;" % query_text,
               ["id", "d"], expected(cat="c3", k=5), float_cols=[1],
               tol=5e-4)
        r.rows(s, "SELECT id, L2_DISTANCE(v,'%s') AS d FROM topk"
                  " WHERE category = 'c3' ORDER BY d DESC LIMIT 5;"
               % query_text,
               ["id", "d"], expected(cat="c3", k=5, desc=True),
               float_cols=[1], tol=5e-4)
        # A filter with fewer than k matches must return only matches.
        r.rows(s, "SELECT id, L2_DISTANCE(v,'%s') AS d FROM topk"
                  " WHERE category = 'none' ORDER BY d LIMIT 5;" % query_text,
               ["id", "d"], [])


def case_update_delete_no_where(r):
    with r.case("update_delete_no_where") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4));")
        r.silent(s, "INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c');")
        r.silent(s, "UPDATE t SET s = 'z';")
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "z"], ["2", "z"], ["3", "z"]])
        r.silent(s, "DELETE FROM t;")
        r.rows(s, "SELECT id FROM t;", ["id"], [])


def case_txn_failure_then_commit(r):
    with r.case("txn_failure_then_commit") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4));")
        r.silent(s, "BEGIN;")
        r.silent(s, "INSERT INTO t VALUES (1,'a');")
        r.error(s, "INSERT INTO t VALUES (1,'dup');", 1062)
        r.silent(s, "INSERT INTO t VALUES (2,'b');")
        r.silent(s, "COMMIT;")
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "a"], ["2", "b"]])
        # everything failing then COMMIT is a no-op commit
        r.silent(s, "BEGIN;")
        r.error(s, "INSERT INTO t VALUES (1,'x');", 1062)
        r.silent(s, "COMMIT;")
        r.rows(s, "SELECT id FROM t ORDER BY id;", ["id"], [["1"], ["2"]])


def case_txn_rollback(r):
    with r.case("txn_rollback") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4),"
                    " v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'a','[1,0]'),"
                    "(2,'b','[0,1]'),(3,'c','[1,1]');")
        r.silent(s, "BEGIN;")
        r.silent(s, "INSERT INTO t VALUES (4,'d','[2,2]');")
        r.silent(s, "UPDATE t SET s = 'q' WHERE id = 1;")
        r.silent(s, "DELETE FROM t WHERE id = 2;")
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "q"], ["3", "c"], ["4", "d"]])
        r.silent(s, "ROLLBACK;")
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "a"], ["2", "b"], ["3", "c"]])


def case_restart_persistence(r):
    with r.case("restart_persistence") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY,"
                    " s VARCHAR(16), v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'a','[1,0]'),(2,'b','[0,1]');")
        r.silent(s, "BEGIN;")
        r.silent(s, "UPDATE t SET s = 'committed' WHERE id = 1;")
        r.silent(s, "COMMIT;")
        s = r.restart(s, d)
        r.rows(s, "SELECT id,s FROM t ORDER BY id;", ["id", "s"],
               [["1", "committed"], ["2", "b"]])
        r.rows(s, "SELECT id FROM t WHERE s = 'committed';", ["id"],
               [["1"]])


def case_vector_index(r):
    with r.case("vector_index") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2),"
                    " s VARCHAR(2));")
        r.silent(s, "CREATE VECTOR INDEX ix ON t(v);")
        r.error(s, "CREATE VECTOR INDEX ix ON t(v);", 1061,
                contains="already exists")
        r.error(s, "CREATE VECTOR INDEX iy ON t(s);", 1366)
        r.error(s, "CREATE VECTOR INDEX iz ON t(nope);", 1054)
        r.error(s, "CREATE VECTOR INDEX iq ON missing(v);", 1146)
        s = r.restart(s, d)
        r.error(s, "CREATE VECTOR INDEX ix ON t(v);", 1061,
                contains="already exists")
        r.silent(s, "CREATE VECTOR INDEX i2 ON t(v);")


def case_multi_row_atomic_invalid(r):
    with r.case("multi_row_atomic_invalid") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(2));")
        r.error(s, "INSERT INTO t VALUES (1,'ok'),(2,'toolong');", 1406)
        r.rows(s, "SELECT id FROM t;", ["id"], [])
        r.silent(s, "INSERT INTO t VALUES (3,'ok');")
        r.rows(s, "SELECT id,s FROM t;", ["id", "s"], [["3", "ok"]])


def case_select_star_and_empty(r):
    with r.case("select_star_and_empty") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4),"
                    " v VECTOR(2));")
        r.header_only(s, "SELECT * FROM t;", ["id", "s", "v"])
        r.silent(s, "INSERT INTO t VALUES (1,'','[1,2]');")
        r.rows(s, "SELECT * FROM t;", ["id", "s", "v"],
               [["1", "", "[1,2]"]])


def case_type_mismatch(r):
    with r.case("type_mismatch") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4),"
                    " v VECTOR(2));")
        r.silent(s, "INSERT INTO t VALUES (1,'a','[1,0]');")
        r.error(s, "UPDATE t SET s = 5;", 1366)
        r.error(s, "UPDATE t SET id = 2;", 1235)
        r.error(s, "SELECT id FROM t WHERE s = 1;", 1366)
        r.error(s, "SELECT id FROM t WHERE id = 'a';", 1366)


def case_negative_limits(r):
    with r.case("negative_limits") as d:
        s = r.session(d)
        r.silent(s, "CREATE TABLE t (id BIGINT PRIMARY KEY);")
        r.silent(s, "INSERT INTO t VALUES (1),(2);")
        r.error(s, "SELECT id FROM t LIMIT -1;", 1366)
        r.error(s, "SELECT id FROM t LIMIT 1.5;", 1366)
        r.error(s, "SELECT id FROM t LIMIT 99999999999999999999;", 1366)
        r.rows(s, "SELECT id FROM t ORDER BY id LIMIT 1;", ["id"], [["1"]])


CASES = [
    ("bigint_limits", case_bigint_limits),
    ("bigint_overflow", case_bigint_overflow),
    ("varchar_codepoints", case_varchar_codepoints),
    ("vector_validation", case_vector_validation),
    ("duplicate_pk", case_duplicate_pk),
    ("unknown_objects", case_unknown_objects),
    ("unsupported", case_unsupported),
    ("ddl_edges", case_ddl_edges),
    ("insert_column_count", case_insert_column_count),
    ("limit_zero", case_limit_zero),
    ("order_strings", case_order_strings),
    ("order_numeric_precision", case_order_numeric_precision),
    ("tie_break", case_tie_break),
    ("topk_oracle", case_topk_oracle),
    ("update_delete_no_where", case_update_delete_no_where),
    ("txn_failure_then_commit", case_txn_failure_then_commit),
    ("txn_rollback", case_txn_rollback),
    ("restart_persistence", case_restart_persistence),
    ("vector_index", case_vector_index),
    ("multi_row_atomic_invalid", case_multi_row_atomic_invalid),
    ("select_star_and_empty", case_select_star_and_empty),
    ("type_mismatch", case_type_mismatch),
    ("negative_limits", case_negative_limits),
]


# ---------------------------------------------------------------------------
# driver
# ---------------------------------------------------------------------------
MODES = [
    ("default", []),
    ("memory-rows=0", ["--memory-rows", "0"]),
]


def run_suite(bin_path, mode_label, mode_args):
    r = Runner(bin_path, mode_args, mode_label)
    print("=" * 78)
    print("SQL edges: binary=%s mode=%s" % (bin_path, mode_label))
    print()
    for name, fn in CASES:
        before = len(r.failures)
        try:
            fn(r)
        except Exception as exc:
            r.failures.append(Failure(name, "<harness>",
                                      "case completed",
                                      "exception: %r" % (exc,)))
        n_fail = len(r.failures) - before
        if n_fail == 0:
            print("  PASS %s" % name)
        else:
            print("  FAIL %s (%d failing check%s)"
                  % (name, n_fail, "" if n_fail == 1 else "s"))
            for f in r.failures[before:]:
                print("       sql: %s" % f.sql)
                print("       expected: %r" % (f.expected,))
                print("       observed: %r" % (f.observed,))
    print()
    print("  checks=%d failures=%d" % (r.checks, len(r.failures)))
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", action="append", default=None,
                    help="binary path (repeatable); default: both existing "
                         "build/minivectordb and build-lead/minivectordb")
    ap.add_argument("--mode", choices=["both", "memory", "disk"],
                    default="both")
    args = ap.parse_args()

    bins = args.bin
    if not bins:
        bins = [p for p in ("./build/minivectordb", "./build-lead/minivectordb")
                if os.path.exists(p)]
    if not bins:
        raise SystemExit("no binary found; pass --bin PATH")

    modes = MODES
    if args.mode == "memory":
        modes = MODES[:1]
    elif args.mode == "disk":
        modes = MODES[1:]

    total_fail = 0
    for bin_path in bins:
        for label, extra in modes:
            r = run_suite(bin_path, label, extra)
            total_fail += len(r.failures)
    print("=" * 78)
    print("total failures: %d" % total_fail)
    return 1 if total_fail else 0


if __name__ == "__main__":
    sys.exit(main())
