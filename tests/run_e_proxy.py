#!/usr/bin/env python3
"""Local proxy for the competition performance benchmark.

Runs the database binary inside a 2 vCPU / 2 GiB docker container (swap off),
drives it over the official JSONL stdin/stdout protocol, and measures the three
official quantities for one cold round:

  T_write  wall time of all 100 batch INSERTs (100 rows each, one commit per
           statement) including persistence
  T_query  wall time of the 1000 measured TopK queries (warmup excluded from T)
  B        cgroup v2 io.stat rbytes+wbytes over the whole container run
           (startup + load + index prep + warmup + queries + writeback)
  M        cgroup v2 memory.peak over the whole run (includes page cache)

It then optionally converts them to the official combined indicator using a
baseline JSON file:

  E = 10000 * (E_write + 2*E_query)/3
  E_* = 0.6*f(R_T) + 0.2*f(R_B) + 0.2*f(R_M), f(R)=R/(1+R)

Usage:
  python3 tests/run_e_proxy.py --bin ./build/minivectordb --repeat 5
  python3 tests/run_e_proxy.py --bin ./build/minivectordb --save /tmp/base.json
  python3 tests/run_e_proxy.py --bin ./build/minivectordb --baseline /tmp/base.json

The baseline JSON is the median of the saved rounds.  Absolute numbers are a
local proxy (different driver/hardware than the platform); compare candidates
against the same baseline file on the same machine.
"""
import argparse
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import time

ROWS = 10000
DIM = 128
BATCH = 100
QUERIES = 1000
K = 10
CATS = 50


def gen(rows=ROWS, dim=DIM, batch=BATCH, queries=QUERIES, cats=CATS, seed=12345,
        clustered=False):
    rng = random.Random(seed)
    cn = [f"cat{i:02d}" for i in range(cats)]
    centers = [[rng.uniform(-1.0, 1.0) for _ in range(dim)] for _ in range(20)]

    def vec(c=None):
        if c is None:
            return [rng.uniform(-1.0, 1.0) for _ in range(dim)]
        return [max(-1.0, min(1.0, c[j] + rng.uniform(-0.35, 0.35)))
                for j in range(dim)]

    schema = (f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
              f"content VARCHAR(1024), embedding VECTOR({dim}));")
    batches = []
    row = 0
    while row < rows:
        n = min(batch, rows - row)
        tup = []
        for k in range(n):
            i = row + k + 1
            vv = vec(centers[i % len(centers)] if clustered else None)
            v = "[" + ",".join(f"{x:.6f}" for x in vv) + "]"
            tup.append(f"({i},'{cn[i % cats]}',"
                       f"'document-{i:06d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx',"
                       f"'{v}')")
        batches.append("INSERT INTO docs VALUES " + ",".join(tup) + ";")
        row += n
    qs = []
    for q in range(queries):
        qv = vec(centers[rng.randrange(len(centers))] if clustered else None)
        v = "[" + ",".join(f"{x:.6f}" for x in qv) + "]"
        if q < queries // 2:
            c = cn[rng.randrange(cats)]
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist "
                      f"FROM docs WHERE category = '{c}' ORDER BY dist LIMIT {K};")
        else:
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist "
                      f"FROM docs ORDER BY dist LIMIT {K};")
    return schema, batches, qs


class ContainerDB:
    def __init__(self, image, bin_path, data_dir, cpus, mem, name, mode="exact",
                 extra_args=(), entrypoint=None, env=()):
        self.name = name
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)
        opts = ["--rm", "-i", "--name", name,
                f"--cpus={cpus}", f"--memory={mem}", f"--memory-swap={mem}",
                "-v", f"{os.path.abspath(bin_path)}:/app/minivectordb:ro",
                "-v", f"{os.path.abspath(data_dir)}:/data",
                "-e", f"MVDB_QUERY_MODE={mode}"]
        for item in env:
            opts += ["-e", item]
        if entrypoint:
            opts += ["--entrypoint", entrypoint]
            opts += [image, "--data-dir", "/data"]
        else:
            opts += [image, "/app/minivectordb", "--data-dir", "/data"]
        opts += list(extra_args)
        self.cid = subprocess.run(
            ["docker", "create"] + opts, capture_output=True, text=True,
            check=True).stdout.strip()
        self.cg = find_cgroup(self.cid)
        self.p = subprocess.Popen(["docker", "start", "-ai", self.cid],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1)
        self.qid = 0
        line = self.p.stdout.readline()
        if '"ready"' not in line:
            raise SystemExit("no ready line: " + repr(line))

    def req(self, sql, mode=None):
        self.qid += 1
        obj = {"id": self.qid, "op": "execute", "sql": sql}
        if mode:
            obj["search_mode"] = mode
        self.p.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise SystemExit("db died")
        r = json.loads(line)
        if not r.get("ok"):
            raise SystemExit("request failed: " + line[:300])
        return r

    def io_bytes(self):
        try:
            with open(os.path.join(self.cg, "io.stat")) as f:
                txt = f.read()
        except OSError:
            return None
        r = w = 0
        for line in txt.splitlines():
            for field in line.split()[1:]:
                k, v = field.split("=")
                if k == "rbytes":
                    r += int(v)
                elif k == "wbytes":
                    w += int(v)
        return r + w

    def mem_peak(self):
        try:
            with open(os.path.join(self.cg, "memory.peak")) as f:
                return int(f.read().strip())
        except OSError:
            return None

    def shutdown(self):
        self.qid += 1
        self.p.stdin.write(json.dumps({"id": self.qid, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        try:
            self.p.stdout.readline()
        except Exception:
            pass
        try:
            self.p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            self.p.kill()


def find_cgroup(cid):
    base = "/sys/fs/cgroup"
    direct = os.path.join(base, "system.slice", f"docker-{cid}.scope")
    if os.path.isdir(direct):
        return direct
    short = cid[:12]
    for root, dirs, _ in os.walk(base):
        if root.count(os.sep) - base.count(os.sep) > 4:
            dirs[:] = []
            continue
        for d in dirs:
            if short in d:
                return os.path.join(root, d)
    return direct


def run_round(args, rep):
    data_dir = os.path.join(args.tmpdir, f"data{rep}")
    shutil.rmtree(data_dir, ignore_errors=True)
    os.makedirs(data_dir)
    schema, batches, qs = gen(clustered=args.clustered)
    name = f"mvdb-e-proxy-{os.getpid()}-{rep}"
    db = ContainerDB(args.image, args.bin, data_dir, args.cpus, args.mem, name,
                     mode=args.mode, env=args.env)
    try:
        t0 = time.perf_counter()
        db.req(schema)
        for b in batches:
            db.req(b)
        tw = time.perf_counter() - t0

        for q in qs[:args.warmup]:
            db.req(q, args.mode)
        t0 = time.perf_counter()
        for q in qs:
            db.req(q, args.mode)
        tq = time.perf_counter() - t0

        b = db.io_bytes()
        m = db.mem_peak()
    finally:
        db.shutdown()
        subprocess.run(["docker", "rm", "-f", name], capture_output=True)
    return {"T_write_ms": tw * 1000.0, "T_query_ms": tq * 1000.0,
            "B_mb": None if b is None else b / 1e6, "M_mb": None if m is None else m / 1e6}


def f(ratio):
    return ratio / (1.0 + ratio)


def combined(baseline, cand):
    eps_t, eps_b, eps_m = 0.001, 1.0, 1.0  # 1 ms, 1 MiB, 1 MiB
    out = {}
    for proj in ("write", "query"):
        t = cand[f"T_{proj}_ms"] / 1000.0
        tb = baseline[f"T_{proj}_ms"] / 1000.0
        b = cand["B_mb"]
        bb = baseline["B_mb"]
        m = cand["M_mb"]
        mb = baseline["M_mb"]
        e = (0.6 * f((tb + eps_t) / (t + eps_t))
             + 0.2 * f((bb + eps_b) / (b + eps_b))
             + 0.2 * f((mb + eps_m) / (m + eps_m)))
        out[proj] = e
    out["E"] = 10000.0 * (out["write"] + 2.0 * out["query"]) / 3.0
    return out


def med(values, digits=3):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return round(statistics.median(vals), digits)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    ap.add_argument("--image", default="ubuntu:24.04")
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--mode", default="exact")
    ap.add_argument("--cpus", type=int, default=2)
    ap.add_argument("--mem", default="2g")
    ap.add_argument("--warmup", type=int, default=50)
    ap.add_argument("--clustered", action="store_true")
    ap.add_argument("--env", action="append", default=[],
                    help="container env var k=v (repeatable)")
    ap.add_argument("--save", default="")
    ap.add_argument("--baseline", default="")
    args = ap.parse_args()

    args.tmpdir = "/tmp/mvdb_e_proxy"
    shutil.rmtree(args.tmpdir, ignore_errors=True)
    os.makedirs(args.tmpdir)

    rounds = []
    for rep in range(args.repeat):
        r = run_round(args, rep)
        rounds.append(r)
        print(f"rep{rep}: T_write={r['T_write_ms']:.1f}ms "
              f"T_query={r['T_query_ms']:.1f}ms B={r['B_mb']:.3f}MB "
              f"M={r['M_mb']:.3f}MB")

    agg = {k: med([r[k] for r in rounds]) for k in rounds[0]}
    print("median: " + json.dumps(agg))
    if args.save:
        with open(args.save, "w") as fp:
            json.dump(agg, fp, indent=1)
        print("saved " + args.save)
    if args.baseline:
        with open(args.baseline) as fp:
            base = json.load(fp)
        c = combined(base, agg)
        print("base:   " + json.dumps(base))
        print(f"E_write={c['write']:.6f} E_query={c['query']:.6f} "
              f"E={c['E']:.6f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
