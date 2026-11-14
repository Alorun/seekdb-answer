#!/usr/bin/env python3
"""UPDATE changed-row semantics and unaliased L2_DISTANCE headers.

MySQL's default affected_rows for UPDATE counts changed rows, not matched
rows; a no-op UPDATE must report 0.  For a projection without an alias MySQL
uses the expression text exactly as written, including spacing and quotes.
"""
import json, os, shutil, subprocess, sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
DIR = "/tmp/mvdb_semantics_fixes"
fails = []

def start():
    p = subprocess.Popen([BIN, "--data-dir", DIR], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1)
    assert '"ready"' in p.stdout.readline()
    return p

def req(p, qid, sql):
    p.stdin.write(json.dumps({"id": qid, "op": "execute", "sql": sql},
                             separators=(",", ":")) + "\n")
    p.stdin.flush()
    ln = p.stdout.readline()
    if not ln:
        raise SystemExit("db died: " + p.stderr.read()[:800])
    r = json.loads(ln)
    if not r.get("ok"):
        raise SystemExit(f"SQL failed: {sql}: {ln}")
    return r

def shutdown(p, qid):
    p.stdin.write(json.dumps({"id": qid, "op": "shutdown"},
                             separators=(",", ":")) + "\n")
    p.stdin.flush()
    p.stdout.readline()
    p.wait(timeout=60)

def check(name, got, want):
    if got != want:
        fails.append((name, got, want))
        print("FAIL", name, got, "!=", want)

def main():
    shutil.rmtree(DIR, ignore_errors=True)
    os.makedirs(DIR)
    p = start()
    req(p, 1, "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16), n BIGINT);")
    check("insert", req(p, 2, "INSERT INTO t VALUES (1,'a',1),(2,'b',2);")["affected_rows"], 2)
    check("noop-int", req(p, 3, "UPDATE t SET n=1 WHERE id=1;")["affected_rows"], 0)
    check("change-int", req(p, 4, "UPDATE t SET n=2 WHERE id=1;")["affected_rows"], 1)
    check("noop-str", req(p, 5, "UPDATE t SET s='b' WHERE id=2;")["affected_rows"], 0)
    check("change-str", req(p, 6, "UPDATE t SET s='c',n=7 WHERE id=2;")["affected_rows"], 1)
    check("no-match", req(p, 7, "UPDATE t SET n=9 WHERE id=99;")["affected_rows"], 0)
    req(p, 8, "BEGIN;")
    check("tx-noop", req(p, 9, "UPDATE t SET n=2 WHERE id=1;")["affected_rows"], 0)
    req(p, 10, "ROLLBACK;")
    rows = req(p, 11, "SELECT id,s,n FROM t ORDER BY id;")["rows"]
    check("rows-after-tx", rows, [[1, "a", 2], [2, "c", 7]])
    req(p, 12, "CREATE TABLE vec (id BIGINT PRIMARY KEY, e VECTOR(2));")
    req(p, 13, "INSERT INTO vec VALUES (1,'[1,2]'),(2,'[3,4]');")
    check("vec-noop", req(p, 14, "UPDATE vec SET e='[1,2]' WHERE id=1;")["affected_rows"], 0)
    check("vec-change", req(p, 15, "UPDATE vec SET e='[2,2]' WHERE id=1;")["affected_rows"], 1)
    # unaliased header keeps the exact expression text (no space, with space)
    r = req(p, 16, "SELECT L2_DISTANCE(e,'[1,2]') FROM vec ORDER BY id;")
    check("header-tight", r["columns"], ["L2_DISTANCE(e,'[1,2]')"])
    r = req(p, 17, "SELECT L2_DISTANCE(e, '[1,2]') FROM vec ORDER BY id;")
    check("header-space", r["columns"], ["L2_DISTANCE(e, '[1,2]')"])
    r = req(p, 18, "SELECT L2_DISTANCE(e,  '[1,2]' ) AS d FROM vec ORDER BY id;")
    check("header-alias", r["columns"], ["d"])
    # shutdown + restart, no-op still 0 and rows persisted
    shutdown(p, 19)
    p = start()
    check("restart-noop", req(p, 20, "UPDATE t SET n=2 WHERE id=1;")["affected_rows"], 0)
    check("restart-rows", req(p, 21, "SELECT id,s,n FROM t ORDER BY id;")["rows"],
          [[1, "a", 2], [2, "c", 7]])
    shutdown(p, 22)
    shutil.rmtree(DIR, ignore_errors=True)
    print("semantics fixes:", "PASS" if not fails else f"FAIL ({len(fails)})")
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
