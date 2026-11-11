#!/usr/bin/env python3
"""Local benchmark approximating the competition write/query workload.

- 10k x 128 rows, batches of 100 (100 commits)
- 1000 JSONL TopK queries, half with category filter, K=10
- warmup queries are sent before the measured query phase
- reports wall time, peak RSS, and /proc/self/io syscall bytes

The client-side cost is included (the official driver's request handling is
also inside T), so compare runs on the same machine/host, not absolute values.

  python3 tools/bench.py --bin ./build/minivectordb --repeat 3
  python3 tools/bench.py --bin ./build/minivectordb --mode ann
"""
import argparse, json, os, random, shutil, subprocess, sys, time

def gen(outdir, rows=10000, dim=128, batch=100, queries=1000, cats=50, seed=12345):
    os.makedirs(outdir, exist_ok=True)
    rng = random.Random(seed)
    cn = [f"cat{i:02d}" for i in range(cats)]
    schema = f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR({dim}));"
    batches = []
    row = 0
    while row < rows:
        n = min(batch, rows - row)
        tup = []
        for k in range(n):
            i = row + k + 1
            v = "[" + ",".join(f"{rng.uniform(-1,1):.6f}" for _ in range(dim)) + "]"
            tup.append(f"({i},'{cn[i%cats]}','document-{i:06d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx','{v}')")
        batches.append("INSERT INTO docs VALUES " + ",".join(tup) + ";")
        row += n
    qs = []
    half = queries // 2
    for q in range(queries):
        v = "[" + ",".join(f"{rng.uniform(-1,1):.6f}" for _ in range(dim)) + "]"
        if q < half:
            c = cn[rng.randrange(cats)]
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs WHERE category = '{c}' ORDER BY dist LIMIT 10;")
        else:
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs ORDER BY dist LIMIT 10;")
    return schema, batches, qs

class DB:
    def __init__(self, bin_path, data_dir, args=(), env=None):
        e = dict(os.environ); e.update(env or {})
        self.p = subprocess.Popen([bin_path, "--data-dir", data_dir] + list(args),
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, env=e, bufsize=1,
                                  text=True)
        line = self.p.stdout.readline()
        if '"ready"' not in line:
            raise SystemExit("no ready: " + repr(line))
        self.qid = 0
        self.maxrss = 0
    def req(self, sql, mode="exact"):
        self.qid += 1
        obj = {"id": self.qid, "op": "execute", "sql": sql, "search_mode": mode}
        self.p.stdin.write(json.dumps(obj, separators=(",",":")) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise SystemExit("db died")
        r = json.loads(line)
        if not r.get("ok"):
            raise SystemExit("request failed: " + line[:300])
        return r
    def sample_rss(self):
        try:
            with open(f"/proc/{self.p.pid}/status") as f:
                for ln in f:
                    if ln.startswith("VmHWM:"):
                        self.maxrss = max(self.maxrss, int(ln.split()[1]))
        except FileNotFoundError:
            pass
    def shutdown(self):
        self.qid += 1
        self.p.stdin.write(json.dumps({"id": self.qid, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()
        self.p.wait(timeout=30)

def io_bytes(pid):
    try:
        d = {}
        with open(f"/proc/{pid}/io") as f:
            for ln in f:
                k, v = ln.split(":")
                d[k] = int(v)
        return d
    except Exception:
        return {}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/minivectordb")
    ap.add_argument("--dir", default="/tmp/mvdb_bench")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--warmup", type=int, default=50)
    ap.add_argument("--mode", default="exact")
    ap.add_argument("--taskset", default="")
    ap.add_argument("--args", default="")
    ap.add_argument("--reuse", action="store_true")
    ap.add_argument("--repeat", type=int, default=1)
    args = ap.parse_args()
    schema, batches, qs = gen(args.dir, rows=args.rows, dim=args.dim, queries=args.queries)
    bin_path = os.path.abspath(args.bin)
    extra = args.args.split() if args.args else []
    for rep in range(args.repeat):
        data_dir = f"/tmp/mvdb_bench_db_{rep}"
        shutil.rmtree(data_dir, ignore_errors=True)
        os.makedirs(data_dir)
        if args.taskset:
            cmd = ["taskset", "-c", args.taskset, os.path.abspath(args.bin)] + extra + ["--data-dir", data_dir]
            db = DB.__new__(DB)
            db.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.DEVNULL, text=True, bufsize=1)
            line = db.p.stdout.readline()
            assert '"ready"' in line, line
            db.qid = 0; db.maxrss = 0
        else:
            db = DB(os.path.abspath(args.bin), data_dir, extra)
        t0 = time.perf_counter()
        db.req(schema)
        for b in batches:
            db.req(b)
        tw = time.perf_counter() - t0
        db.sample_rss()
        io1 = io_bytes(db.p.pid)
        t0 = time.perf_counter()
        for _ in range(args.warmup):
            db.req(qs[_ % len(qs)], args.mode)
        twarm = time.perf_counter() - t0
        db.sample_rss()
        t0 = time.perf_counter()
        for q in qs:
            db.req(q, args.mode)
        tq = time.perf_counter() - t0
        db.sample_rss()
        io2 = io_bytes(db.p.pid)
        db.shutdown()
        db.sample_rss()
        print(f"rep{rep}: write={tw:.4f}s queries={tq:.4f}s warmup={twarm:.4f}s "
              f"maxrss={db.maxrss/1024:.1f}MiB "
              f"io_read={(io2.get('rchar',0)-io1.get('rchar',0))/1e6:.1f}MB "
              f"io_write={(io2.get('wchar',0)-io1.get('wchar',0))/1e6:.1f}MB "
              f"syscr={io2.get('syscr',0)-io1.get('syscr',0)} syscw={io2.get('syscw',0)-io1.get('syscw',0)}")

if __name__ == "__main__":
    main()
