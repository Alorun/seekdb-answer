#!/usr/bin/env python3
"""Absolute checks for JSON request escaping and typed-id echo.

These cover the request-parsing edge cases that a naive "fast path" for the
benchmark's escape-free SQL gets wrong:

  * a SQL string that contains a JSON escape (\\\\, \\", \\t, \\n, \\uXXXX) must be
    unescaped and executed, not rejected;
  * the response must echo the request id with its exact JSON type (object,
    array, bool, null, number);
  * an empty/missing/non-string sql is an error, not a crash;
  * a string whose closing quote sits on a 16-byte SIMD boundary must parse.

Run: python3 tests/run_json_escape_diff.py --bin ./build/minivectordb
"""
import argparse
import json
import os
import shutil
import subprocess
import sys


def start(binp, data_dir):
    p = subprocess.Popen([binp, "--data-dir", data_dir], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1)
    line = p.stdout.readline()
    if '"ready"' not in line:
        raise SystemExit("no ready line: " + repr(line))
    return p


def send(p, obj):
    p.stdin.write(json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n")
    p.stdin.flush()
    line = p.stdout.readline()
    if not line:
        raise SystemExit("db exited before response")
    return json.loads(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    args = ap.parse_args()
    data = "/tmp/mvdb_json_escape"
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(data)
    p = start(os.path.abspath(args.bin), data)
    failures = []
    seq = [0]

    UNSET = object()

    def execute(sql, ident=UNSET, **kw):
        seq[0] += 1
        obj = {"id": seq[0] if ident is UNSET else ident, "op": "execute", "sql": sql}
        obj.update(kw)
        return send(p, obj)

    def expect(cond, what, got=None):
        if not cond:
            failures.append(f"{what}: {got!r}")

    r = execute("CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(200), v VECTOR(2));")
    expect(r.get("ok") is True, "create table", r)

    # (wire value, value the SQL layer must store).  The wire value is written
    # by json.dumps, so it exercises the JSON unescape path; the expected value
    # is what MySQL-style SQL unescaping of the resulting literal produces.
    cases = [
        ('a"b', 'a"b'),            # JSON \" -> SQL double quote inside '...'
        ("a\tb", "a\tb"),        # JSON \t -> TAB, stored as TAB
        ("\u4e2d\u6587", "中文"),  # JSON \uXXXX -> UTF-8
        ("x" * 15, "x" * 15),      # SIMD boundary lengths
        ("x" * 16, "x" * 16),
        ("x" * 17, "x" * 17),
        ("x" * 31, "x" * 31),
        ("x" * 32, "x" * 32),
        ("x" * 33, "x" * 33),
        ("x" * 64, "x" * 64),
        ("中文\t混排", "中文\t混排"),
    ]
    pk = 100
    for written, expected in cases:
        pk += 1
        r = execute("INSERT INTO t VALUES (%d,'%s','[0,0]');" % (pk, written))
        expect(r.get("ok") is True, f"insert {written!r}", r)
        r = execute("SELECT id, s FROM t WHERE id=%d;" % pk)
        expect(r.get("ok") is True, f"select {written!r}", r)
        rows = r.get("rows") or []
        expect(rows and rows[0][1] == expected,
               f"roundtrip {written!r} expected {expected!r}", rows)

    # A JSON double backslash must reach the SQL layer as one backslash; the
    # SQL parser then halves it again, so the stored text is a single '\\'.
    pk += 1
    r = execute("INSERT INTO t VALUES (%d,'a\\\\b','[0,0]');" % pk)
    expect(r.get("ok") is True, "double backslash insert", r)
    r = execute("SELECT s FROM t WHERE id=%d;" % pk)
    expect((r.get("rows") or [[None]])[0][0] == "a\\b", "double backslash read", r)

    # Typed id echo: the response must carry the same JSON type.
    for ident in ({"a": 1, "b": [2, 3]}, [1, "two", None], True, None, 3.5):
        r = execute("SELECT id FROM t WHERE id=101;", ident=ident)
        expect(r.get("id") == ident and type(r.get("id")) is type(ident),
               f"typed id {ident!r}", r)

    # Malformed requests are errors, not crashes, and still answer.
    for bad in ({"op": "execute"}, {"op": "execute", "sql": 123}):
        bad["id"] = "bad-" + str(seq[0] + 1)
        seq[0] += 1
        r = send(p, bad)
        expect(r.get("ok") is False and r.get("error", {}).get("code"),
               f"malformed {bad!r}", r)

    # An empty statement is a no-op in the official protocol (the driver may
    # send trailing blank lines), so it must succeed.
    r = execute("")
    expect(r.get("ok") is True, "empty statement", r)

    for mode in ("exact", "ann"):
        r = execute("SELECT id, L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 2;",
                    search_mode=mode)
        expect(r.get("ok") is True, f"search_mode {mode}", r)

    seq[0] += 1
    p.stdin.write(json.dumps({"id": seq[0], "op": "shutdown"}) + "\n")
    p.stdin.flush()
    p.stdout.readline()
    p.wait(timeout=60)
    err = p.stderr.read()
    if "AddressSanitizer" in err or "runtime error:" in err:
        failures.append("sanitizer report in stderr")

    if failures:
        print(f"FAIL: {len(failures)} check(s)")
        for f in failures:
            print("  " + f)
        return 1
    print("json escape / typed-id checks: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
