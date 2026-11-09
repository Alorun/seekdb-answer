#!/usr/bin/env python3
"""Run the official public MiniVectorDB cases against a binary.

The public pack (tests/public_cases, copied from the official
2026-competition-round2 branch) defines one complete SQL per line and JSON
assertions for affected_rows / columns / rows. This runner assumes the
competition protocol: line framing, `OK <affected>` for DML/DDL, header + rows
for SELECT, and `ERROR ...` for failures.

Usage: python3 tests/run_public_cases.py [--bin ./build/minivectordb]
"""
import argparse
import collections
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from mvdb_harness import Session, new_data_dir, remove_data_dir  # noqa: E402


def parse_value(expected, text):
    if isinstance(expected, bool):
        return text == "true"
    if isinstance(expected, int) and not isinstance(expected, bool):
        return int(text)
    if isinstance(expected, float):
        return float(text)
    return text


def same_value(expected, actual):
    if isinstance(expected, float):
        try:
            got = float(actual)
        except (TypeError, ValueError):
            return False
        return abs(got - expected) <= 1e-5 * max(1.0, abs(expected))
    if isinstance(expected, int) and not isinstance(expected, bool):
        try:
            return int(actual) == expected
        except (TypeError, ValueError):
            return False
    return str(actual) == str(expected)


def run_case(binary, case_dir, name):
    lines = [
        line
        for line in open(os.path.join(case_dir, name + ".test"), encoding="utf-8").read().splitlines()
        if line.strip()
    ]
    expected = json.load(open(os.path.join(case_dir, name + ".result"), encoding="utf-8"))
    data_dir = new_data_dir("mvdb-public-")
    s = None
    failures = []
    mode_args = []
    exp_i = 0

    def start(where):
        nonlocal s
        s = Session(binary, data_dir, args=mode_args)
        if not s.wait_lines(1, 3.0):
            failures.append((where, "missing ready marker"))
            return False
        got = s.take_out().strip()
        if got != "ready":
            failures.append((where, "startup output", got))
            return False
        return True

    try:
        if not start("<start>"):
            return failures
        for raw in lines:
            if raw == "--restart":
                s.close()
                s = None
                if not start("--restart"):
                    break
                continue
            if raw.startswith("--mode"):
                # The mode instruction is a driver directive, not SQL. Exact
                # fallback produces the same rows for the public small data.
                if raw.strip() == "--mode ann":
                    mode_args = ["--query-mode", "ann"]
                continue
            if exp_i >= len(expected):
                failures.append((raw, "extra SQL without assertion"))
                continue
            exp = expected[exp_i]
            exp_i += 1
            s.send(raw + "\n")
            if "error" in exp:
                if not s.wait_lines(1, 2.0):
                    failures.append((raw, "timeout waiting for error", s.out_text()))
                    s.take_out()
                    continue
                line = s.take_out().splitlines()[0] if s.out_text().splitlines() else ""
                if not line.startswith("ERROR "):
                    failures.append((raw, "expected error", exp["error"], line))
                continue
            if "columns" in exp:
                need = 1 + len(exp["rows"])
                if not s.wait_lines(need, 2.0):
                    failures.append((raw, "timeout waiting for rows", s.out_text()))
                    s.take_out()
                    continue
                lines_out = s.take_out().splitlines()
                header = lines_out[0].split("\t") if lines_out else []
                if header != exp["columns"]:
                    failures.append((raw, "columns", exp["columns"], header))
                body = lines_out[1:]
                if len(body) != len(exp["rows"]):
                    failures.append((raw, "row count", len(exp["rows"]), len(body)))
                    continue
                typed = []
                for expected_row, actual_row in zip(exp["rows"], body):
                    fields = actual_row.split("\t")
                    if len(fields) != len(expected_row):
                        failures.append((raw, "field count", expected_row, fields))
                        typed = []
                        break
                    typed.append([parse_value(e, a) for e, a in zip(expected_row, fields)])
                if not typed:
                    continue
                if exp.get("unordered"):
                    want = collections.Counter(tuple(r) for r in exp["rows"])
                    got = collections.Counter(tuple(r) for r in typed)
                    if want != got:
                        failures.append((raw, "unordered rows", exp["rows"], typed))
                else:
                    for expected_row, actual_row in zip(exp["rows"], typed):
                        if not all(same_value(e, a) for e, a in zip(expected_row, actual_row)):
                            failures.append((raw, "rows", exp["rows"], typed))
                            break
            else:
                if not s.wait_lines(1, 2.0):
                    failures.append((raw, "timeout waiting for status", s.out_text()))
                    s.take_out()
                    continue
                line = s.take_out().splitlines()[0] if s.out_text().splitlines() else ""
                if line.startswith("ERROR"):
                    failures.append((raw, "unexpected error", line))
                    continue
                if not line.startswith("OK"):
                    failures.append((raw, "status", line))
                    continue
                if "affected_rows" in exp:
                    try:
                        got = int(line.split()[1])
                    except (IndexError, ValueError):
                        got = None
                    if got != exp["affected_rows"]:
                        failures.append((raw, "affected", exp["affected_rows"], line))
    finally:
        if s:
            s.close()
        remove_data_dir(data_dir)
    if exp_i != len(expected):
        failures.append(("<end>", "assertion count", len(expected), exp_i))
    return failures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "minivectordb"))
    ap.add_argument("--cases", default=os.path.join(ROOT, "tests", "public_cases", "cases"))
    args = ap.parse_args()
    manifest_path = os.path.join(os.path.dirname(args.cases), "manifest.json")
    names = [c["id"] for c in json.load(open(manifest_path))["cases"]]
    total = 0
    for name in names:
        failures = run_case(os.path.abspath(args.bin), args.cases, name)
        print(("PASS " if not failures else "FAIL ") + name)
        for f in failures[:5]:
            print("  ", f)
        total += len(failures)
    print(f"public cases: {len(names)} cases, {total} failure(s)")
    return 0 if total == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
