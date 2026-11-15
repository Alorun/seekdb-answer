#!/usr/bin/env python3
"""Run N fresh-directory rounds and report the wall-time spread."""
import argparse, re, statistics, subprocess

ap = argparse.ArgumentParser()
ap.add_argument("--rounds", type=int, default=5)
ap.add_argument("--bin", default="./build/minivectordb")
ap.add_argument("--mode", default="all")
A = ap.parse_args()
out = subprocess.run(["python3", "tools/bench.py", "--bin", A.bin, "--taskset", "8,10",
                      "--repeat", str(A.rounds)], capture_output=True, text=True)
lines = [l for l in out.stdout.splitlines() if l.startswith("rep")]
w = [float(re.search(r"write=([0-9.]+)s", l).group(1)) * 1000 for l in lines]
q = [float(re.search(r"queries=([0-9.]+)s", l).group(1)) * 1000 for l in lines]
def report(name, xs):
    p50 = statistics.median(xs)
    print(f"{name} min={min(xs):.1f} p50={p50:.1f} max={max(xs):.1f} "
          f"spread={(max(xs)-min(xs))/p50*100:.1f}%")
report("write_ms", w); report("query_ms", q)
print(f"rounds={len(w)} noise_band_2x_halfwidth_ms={(max(w)-min(w))/2:.1f}")
