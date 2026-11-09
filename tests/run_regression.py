#!/usr/bin/env python3
"""Regression suite for the local MiniVectorDB build.

Usage: python3 tests/run_regression.py ./build/minivectordb
"""
import math
import os
import random
import shutil
import subprocess
import sys
import tempfile

BIN = ""
FAILS = []


def run(sql, data_dir, extra=None, env=None, timeout=30):
    e = dict(os.environ)
    e.update(env or {})
    p = subprocess.Popen(
        [BIN, "--data-dir", data_dir, "--dml-ack=none",
         "--statement-mode", "semicolon"] + (extra or []),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=e,
    )
    out, err = p.communicate(sql, timeout=timeout)
    return p.returncode, out, err


def check(name, cond, detail=""):
    if cond:
        print("ok   " + name)
    else:
        print("FAIL " + name)
        if detail:
            print(detail)
        FAILS.append(name)


def has(out, needle):
    return needle in out


def test_basic():
    d = tempfile.mkdtemp(prefix="mvdb-reg-basic-")
    try:
        sql = """
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  category VARCHAR(32),
  content VARCHAR(1024),
  embedding VECTOR(4)
);
INSERT INTO docs VALUES
 (1,'database','事务与恢复','[1,0,0,0]'),
 (2,'database','向量索引','[0,1,0,0]'),
 (3,'travel','旅行攻略','[0,0,1,0]');
SELECT id, content, L2_DISTANCE(embedding, '[0.9,0.1,0,0]') AS distance
 FROM docs WHERE category = 'database' ORDER BY distance LIMIT 2;
BEGIN;
UPDATE docs SET embedding='[0.8,0.2,0,0]' WHERE id=1;
DELETE FROM docs WHERE id=2;
ROLLBACK;
SELECT id, content FROM docs ORDER BY id;
"""
        for extra in ([], ["--memory-rows", "0"]):
            tag = "disk" if extra else "memory"
            rc, out, err = run(sql, d, extra)
            check(f"basic-{tag}-rc", rc == 0, err)
            check(f"basic-{tag}-ready", out.startswith("ready\n"), out)
            check(f"basic-{tag}-header", "id\tcontent\tdistance" in out, out)
            check(f"basic-{tag}-row1", "1\t事务与恢复" in out, out)
            check(f"basic-{tag}-row2", "2\t向量索引" in out, out)
            check(f"basic-{tag}-row3", "3\t旅行攻略" in out, out)
            rc, out, err = run("SELECT id FROM docs ORDER BY id;\n", d, extra)
            check(f"basic-{tag}-restart", "1\n2\n3\n" in out, out)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_truth():
    rng = random.Random(42)
    n = 300
    dim = 8
    rows = []
    for i in range(1, n + 1):
        vec = [rng.uniform(-1, 1) for _ in range(dim)]
        cat = f"c{i % 7}"
        rows.append((i, cat, vec))
    query = [rng.uniform(-1, 1) for _ in range(dim)]
    vec_text = "[" + ",".join(f"{x:.6f}" for x in query) + "]"

    def truth(filter_cat=None):
        cand = []
        for pk, cat, vec in rows:
            if filter_cat is not None and cat != filter_cat:
                continue
            d = math.sqrt(sum((a - b) ** 2 for a, b in zip(vec, query)))
            cand.append((d, pk))
        cand.sort()
        return [pk for _, pk in cand[:10]]

    d = tempfile.mkdtemp(prefix="mvdb-reg-truth-")
    try:
        sql = ["CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(8), v VECTOR(8));"]
        for b in range(0, n, 100):
            sql.append(
                "INSERT INTO t VALUES "
                + ",".join(
                    f"({pk},'{cat}','["
                    + ",".join(f"{x:.6f}" for x in vec)
                    + "]')"
                    for pk, cat, vec in rows[b : b + 100]
                )
                + ";"
            )
        for extra in ([], ["--memory-rows", "0"]):
            tag = "disk" if extra else "memory"
            d2 = tempfile.mkdtemp(prefix="mvdb-reg-truth-data-")
            try:
                rc, out, err = run("\n".join(sql) + "\n", d2, extra)
                check(f"truth-{tag}-load", rc == 0, err)
                q1 = (
                    f"SELECT id, L2_DISTANCE(v,'{vec_text}') AS d FROM t "
                    "ORDER BY d LIMIT 10;"
                )
                rc, out, err = run(q1 + "\n", d2, extra)
                got = [int(line.split("\t")[0]) for line in out.splitlines()[2:] if line]
                check(f"truth-{tag}-unfiltered", got == truth(), f"{got} != {truth()}\n{out}")
                q2 = (
                    f"SELECT id FROM t WHERE category = 'c3' "
                    f"ORDER BY L2_DISTANCE(v,'{vec_text}') LIMIT 10;"
                )
                # The parser accepts ORDER BY alias; use explicit alias for this build.
                q2 = (
                    f"SELECT id, L2_DISTANCE(v,'{vec_text}') AS d FROM t "
                    "WHERE category = 'c3' ORDER BY d LIMIT 10;"
                )
                rc, out, err = run(q2 + "\n", d2, extra)
                got = [int(line.split("\t")[0]) for line in out.splitlines()[2:] if line]
                check(f"truth-{tag}-filtered", got == truth("c3"), f"{got} != {truth('c3')}\n{out}")
            finally:
                shutil.rmtree(d2, ignore_errors=True)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_transactions():
    d = tempfile.mkdtemp(prefix="mvdb-reg-txn-")
    try:
        sql = """
CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));
BEGIN;
INSERT INTO t VALUES (1,'a');
SELECT id FROM t;
ROLLBACK;
SELECT id FROM t;
BEGIN;
INSERT INTO t VALUES (2,'b');
COMMIT;
BEGIN;
INSERT INTO t VALUES (2,'c');
DELETE FROM t WHERE id=2;
ROLLBACK;
SELECT id, s FROM t ORDER BY id;
"""
        for extra in ([], ["--memory-rows", "0"]):
            tag = "disk" if extra else "memory"
            rc, out, err = run(sql, d, extra)
            check(f"txn-{tag}-rc", rc == 0, err)
            check(f"txn-{tag}-rollback", "\nid\n\n" in out or "id\n" in out, out)
            check(f"txn-{tag}-commit", "2\tb" in out, out)
            check(f"txn-{tag}-no-c", "2\tc" not in out, out)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_compat():
    for extra in ([], ["--memory-rows", "0"]):
        tag = "disk" if extra else "memory"
        d = tempfile.mkdtemp(prefix="mvdb-reg-compat-")
        try:
            sql = """
SET NAMES utf8mb4;
USE test;
SET autocommit=0;
CREATE TABLE IF NOT EXISTS t (id BIGINT PRIMARY KEY, s VARCHAR(8));
INSERT INTO t VALUES (1,'a'),(2,'b');
TRUNCATE TABLE t;
SELECT id FROM t;
DROP TABLE IF EXISTS t;
DROP TABLE IF EXISTS missing;
CREATE TABLE t (id BIGINT PRIMARY KEY);
INSERT INTO t VALUES (3);
SELECT id FROM t;
"""
            rc, out, err = run(sql, d, extra)
            check(f"compat-{tag}-rc", rc == 0, err)
            check(f"compat-{tag}-truncate", "id\n3\n" in out, out)
            check(f"compat-{tag}-drop-missing", "ERROR 1146" not in out, out)
            rc, out, err = run("SELECT id FROM t;\n", d, extra)
            check(f"compat-{tag}-restart", "3" in out, out)
        finally:
            shutil.rmtree(d, ignore_errors=True)


def test_order_limit():
    for extra in ([], ["--memory-rows", "0"]):
        tag = "disk" if extra else "memory"
        d = tempfile.mkdtemp(prefix="mvdb-reg-order-")
        try:
            sql = """
CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4));
INSERT INTO t VALUES (3,'b'),(1,'a'),(2,'a');
SELECT id FROM t ORDER BY s LIMIT 2;
SELECT id FROM t ORDER BY id DESC LIMIT 2;
SELECT id FROM t LIMIT 2;
"""
            rc, out, err = run(sql, d, extra)
            sections = out.split("id")
            check(f"order-{tag}-asc", "1\n2" in out, out)
            check(f"order-{tag}-desc", "3\n2" in out, out)
            check(f"order-{tag}-limit", len(out.splitlines()) >= 9, out)
        finally:
            shutil.rmtree(d, ignore_errors=True)


def test_errors():
    d = tempfile.mkdtemp(prefix="mvdb-reg-err-")
    try:
        sql = """
CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(2), v VECTOR(2));
INSERT INTO t VALUES (1,'你好','[1,2]');
INSERT INTO t VALUES (2,'你好啊','[1,2]');
INSERT INTO t VALUES (3,'x','[1]');
INSERT INTO t VALUES (4,'x','[1e39,1]');
INSERT INTO t VALUES (1,'x','[1,2]');
SELECT id FROM t ORDER BY id;
"""
        rc, out, err = run(sql, d)
        check("err-rc", rc == 0, err)
        check("err-varchar", "ERROR 1406" in out, out)
        check("err-dim", "ERROR 1366" in out, out)
        check("err-overflow", "ERROR 1366" in out, out)
        check("err-dup", "ERROR 1062" in out, out)
        check("err-survives", "1\n2" not in out, out)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_crash():
    def start(d, sql, env=None, extra=None):
        e = dict(os.environ)
        e.update(env or {})
        p = subprocess.Popen(
            [BIN, "--data-dir", d, "--dml-ack=none",
             "--statement-mode", "semicolon"] + (extra or []),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=e,
        )
        p.stdin.write(sql)
        p.stdin.flush()
        return p

    d = tempfile.mkdtemp(prefix="mvdb-reg-crash-")
    try:
        rc, out, err = run("CREATE TABLE t (id BIGINT PRIMARY KEY);\n", d)
        # Kill after a WAL record but before COMMIT: no row.
        p = start(d, "INSERT INTO t VALUES (1);\n", {"MVDB_CRASH_POINT": "after_wal_write"})
        p.communicate(timeout=15)
        rc, out, err = run("SELECT id FROM t;\n", d)
        check("crash-uncommitted", "1" not in out, out)
        # Kill after COMMIT fsync: row must survive.
        p = start(d, "INSERT INTO t VALUES (2);\n", {"MVDB_CRASH_POINT": "after_commit_fsync"})
        p.communicate(timeout=15)
        rc, out, err = run("SELECT id FROM t ORDER BY id;\n", d)
        check("crash-committed", "2" in out, out)
        # Kill during recovery after pass 1, then recover again.
        rc, out, err = run("SELECT id FROM t;\n", d,
                           env={"MVDB_CRASH_POINT": "recovery_after_pass1"})
        check("crash-recovery-kill", rc == 137, f"rc={rc}")
        rc, out, err = run("SELECT id FROM t;\n", d)
        check("crash-recovery-retry", "2" in out, out)
        # Kill during checkpoint before rename: old WAL remains valid.
        rc, out, err = run("INSERT INTO t VALUES (3);\n", d,
                           extra=["--checkpoint-bytes", "1"],
                           env={"MVDB_CRASH_POINT": "checkpoint_before_rename"})
        check("crash-checkpoint-kill", rc == 137, f"rc={rc}")
        rc, out, err = run("SELECT id FROM t ORDER BY id;\n", d)
        check("crash-checkpoint-old-wal", "3" in out, out)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_index_and_set():
    d = tempfile.mkdtemp(prefix="mvdb-reg-index-")
    try:
        sql = """
CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));
INSERT INTO t VALUES (1,'[1,0]'),(2,'[0,1]');
CREATE VECTOR INDEX ix ON t(v);
CREATE VECTOR INDEX ix ON t(v);
SET query_mode='ann';
INSERT INTO t VALUES (3,'[1,1]');
UPDATE t SET v='[9,9]' WHERE id=1;
DELETE FROM t WHERE id=2;
SELECT id FROM t ORDER BY id;
"""
        rc, out, err = run(sql, d)
        check("index-rc", rc == 0, err)
        check("index-dup", "ERROR 1061" in out, out)
        check("index-rows", "1\n3" in out, out)
        rc, out, err = run("CREATE VECTOR INDEX ix ON t(v);\n", d)
        check("index-dup-restart", "ERROR 1061" in out, out)
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    global BIN
    BIN = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb")
    if not os.path.exists(BIN):
        raise SystemExit("binary not found: " + BIN)
    test_basic()
    test_truth()
    test_transactions()
    test_compat()
    test_order_limit()
    test_errors()
    test_crash()
    test_index_and_set()
    print()
    if FAILS:
        print(f"{len(FAILS)} failure(s): {FAILS}")
        raise SystemExit(1)
    print("all regression tests passed")


if __name__ == "__main__":
    main()
