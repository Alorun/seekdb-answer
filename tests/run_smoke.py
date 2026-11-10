#!/usr/bin/env python3
"""Local smoke tests for the v0.1 MiniVectorDB binary.

Run after building:
    python3 tests/run_smoke.py ./build/minivectordb
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time


def run(binary, data_dir, sql, dml_ack=True, timeout=15, extra=None):
    args = [binary, "--data-dir", data_dir, "--protocol", "text"]
    if dml_ack:
        args.append("--dml-ack")
    args += ["--statement-mode", "semicolon"]
    if extra:
        args.extend(extra)
    p = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    out, err = p.communicate(sql, timeout=timeout)
    if p.returncode != 0:
        raise AssertionError(
            f"process exited {p.returncode}\nstdout:\n{out}\nstderr:\n{err}"
        )
    return out, err


def check_distance(out, prefix, expected):
    for line in out.splitlines():
        if line.startswith(prefix + "\t"):
            got = float(line.split("\t")[-1])
            if abs(got - expected) > 1e-5:
                raise AssertionError(
                    f"distance mismatch for {prefix!r}: got {got}, expected {expected}\n--- output ---\n{out}"
                )
            return
    raise AssertionError(f"missing distance row {prefix!r}\n--- output ---\n{out}")


def check(cond, msg, out=""):
    if not cond:
        raise AssertionError(msg + ("\n--- output ---\n" + out if out else ""))


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/minivectordb"
    binary = os.path.abspath(binary)
    if not os.path.exists(binary):
        raise SystemExit(f"binary not found: {binary}")

    root = tempfile.mkdtemp(prefix="mvdb-smoke-")
    try:
        sql = open(os.path.join(os.path.dirname(__file__), "smoke.sql"), encoding="utf-8").read()
        # Run the same suite in memory mode and forced disk mode.
        for extra in ([], ["--memory-rows", "0"]):
            out, err = run(binary, root, sql, extra=extra)
            check(out.startswith("ready\n"), "missing ready marker", out)
            check("id\tcontent\tdistance" in out, "missing distance header", out)
            check_distance(out, "1\t事务与恢复", 0.141421356)
            check_distance(out, "2\t向量索引", 1.27279221)
            tail = out.split("id\tcontent\n")[-1]
            check("1\t事务与恢复" in tail, "row 1 missing after rollback", out)
            check("2\t向量索引" in tail, "row 2 missing after rollback", out)

            # Restart: committed data must survive a clean shutdown.
            out2, err2 = run(binary, root, "SELECT id, content FROM docs ORDER BY id;\n",
                             extra=extra)
            check(out2.startswith("ready\n"), "missing ready on restart", out2)
            check("1\t事务与恢复" in out2, "row 1 lost after restart", out2)
            check("3\t旅行攻略" in out2, "row 3 lost after restart", out2)

        # Crash: kill -9 after the insert commit, then recover.
        crash_dir = tempfile.mkdtemp(prefix="mvdb-crash-")
        try:
            p = subprocess.Popen(
                [binary, "--data-dir", crash_dir, "--protocol", "text",
                 "--dml-ack", "--statement-mode", "semicolon"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            p.stdin.write(
                "CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));\n"
                "INSERT INTO t VALUES (7, '[1,2]');\n"
            )
            p.stdin.flush()
            time.sleep(0.5)
            p.kill()
            p.wait(timeout=5)

            out3, err3 = run(binary, crash_dir, "SELECT id FROM t ORDER BY id;\n")
            check(out3.startswith("ready\n"), "crash restart failed", out3)
            check("7" in out3, "committed row lost after kill -9", out3)
        finally:
            shutil.rmtree(crash_dir, ignore_errors=True)

        print("smoke tests passed")
    finally:
        shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
