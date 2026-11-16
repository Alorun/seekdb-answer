#!/usr/bin/env python3
"""Interleaved A/B of two MiniVectorDB binaries on the official-shape workload.

Runs the SAME 10k x 128 / 1000-query workload through a 2 vCPU / 2 GiB docker
container for each binary, alternating rounds, and reports medians plus the
official combined indicator where the "baseline" is binary A.

  python3 scripts/ab_two_bins.py --a /tmp/base/build/minivectordb \
      --b ./build/minivectordb --rounds 5
"""
import argparse
import json
import os
import shutil
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tests"))
import run_e_proxy as ep  # noqa: E402


class DB:
    def __init__(self, binp, data_dir, name, mode, env):
        self.inner = ep.ContainerDB("ubuntu:24.04", binp, data_dir, 2, "2g", name,
                                    mode=mode, env=env)
        self.qid = 0

    def req(self, sql, mode):
        self.qid += 1
        obj = {"id": self.qid, "op": "execute", "sql": sql, "search_mode": mode}
        self.inner.p.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
        self.inner.p.stdin.flush()
        line = self.inner.p.stdout.readline()
        if not line:
            raise SystemExit("db died")
        return json.loads(line)

    def shutdown(self):
        self.inner.shutdown()


def one_round(binp, tag, round_id, args, schema, batches, qs):
    data_dir = f"/tmp/ab_two_{tag}_{round_id}"
    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir)
    db = DB(os.path.abspath(binp), data_dir, f"abtwo-{tag}-{os.getpid()}-{round_id}",
            args.mode, args.env)
    import time
    t0 = time.perf_counter()
    db.req(schema, "exact")
    for b in batches:
        db.req(b, "exact")
    tw = time.perf_counter() - t0
    for q in qs[:args.warmup]:
        db.req(q, args.mode)
    t0 = time.perf_counter()
    for q in qs:
        db.req(q, args.mode)
    tq = time.perf_counter() - t0
    b = db.inner.io_bytes()
    m = db.inner.mem_peak()
    db.shutdown()
    return {"T_write_ms": tw * 1000, "T_query_ms": tq * 1000,
            "B_mb": (b or 0) / 1e6, "M_mb": (m or 0) / 1e6}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--mode", default="exact")
    ap.add_argument("--warmup", type=int, default=50)
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    schema, batches, qs = ep.gen()
    res = {"a": [], "b": []}
    for i in range(args.rounds):
        order = [("a", args.a), ("b", args.b)] if i % 2 == 0 else [("b", args.b), ("a", args.a)]
        for tag, binp in order:
            r = one_round(binp, tag, i, args, schema, batches, qs)
            res[tag].append(r)
            print(f"round{i} {tag}: T_write={r['T_write_ms']:.1f} T_query={r['T_query_ms']:.1f} "
                  f"B={r['B_mb']:.3f} M={r['M_mb']:.3f}", flush=True)
    med = {t: {k: round(statistics.median([r[k] for r in res[t]]), 3) for k in res[t][0]}
           for t in res}
    print("median A:", json.dumps(med["a"]))
    print("median B:", json.dumps(med["b"]))
    e = ep.combined(med["a"], med["b"])
    print(f"E vs A: E_write={e['write']:.6f} E_query={e['query']:.6f} E={e['E']:.6f}")
    print("deltas B vs A: "
          f"T_write={100*(med['b']['T_write_ms']/med['a']['T_write_ms']-1):+.2f}% "
          f"T_query={100*(med['b']['T_query_ms']/med['a']['T_query_ms']-1):+.2f}% "
          f"B={100*(med['b']['B_mb']/med['a']['B_mb']-1):+.2f}% "
          f"M={100*(med['b']['M_mb']/med['a']['M_mb']-1):+.2f}%")
    if args.out:
        with open(args.out, "w") as fp:
            json.dump({"a": med["a"], "b": med["b"], "rounds": res, "E": e}, fp, indent=1)


if __name__ == "__main__":
    main()
