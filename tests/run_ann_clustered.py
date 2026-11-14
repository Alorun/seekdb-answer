#!/usr/bin/env python3
"""Clustered ann-mode recall/latency, including UPDATE/DELETE and restart.

Builds a 10k x 128 clustered table, CREATE VECTOR INDEX, and compares ann
results against a NumPy exact oracle.  The q7 IVF index is selected only when
the data is clusterable; the self-check falls back to exact IVF otherwise.
"""
import argparse, json, os, random, shutil, subprocess, sys, time
import numpy as np

def lit(v):
    return "[" + ",".join(f"{x:.6f}" for x in v) + "]"

class DB:
    def __init__(self, binp, d):
        self.p = subprocess.Popen([binp, "--data-dir", d], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, bufsize=1)
        assert '"ready"' in self.p.stdout.readline()
        self.qid = 0
    def req(self, sql, mode="exact"):
        self.qid += 1
        self.p.stdin.write(json.dumps({"id": self.qid, "op": "execute", "sql": sql,
                                       "search_mode": mode},
                                      separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        ln = self.p.stdout.readline()
        if not ln:
            raise SystemExit("db died: " + self.p.stderr.read()[:800])
        r = json.loads(ln)
        if not r.get("ok"):
            raise SystemExit(f"SQL failed: {sql[:80]}: {ln[:300]}")
        return r
    def shutdown(self):
        self.qid += 1
        self.p.stdin.write(json.dumps({"id": self.qid, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()
        self.p.wait(timeout=60)

def oracle(data, pks, q, k=10):
    d = np.sum((data - q.astype(np.float32)) ** 2, axis=1)
    o = np.lexsort((pks, d))[:k]
    return pks[o].tolist()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--dim", type=int, default=128)
    args = ap.parse_args()
    binp = os.path.abspath(args.bin)
    d = "/tmp/mvdb_ann_clustered"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    rng = np.random.default_rng(11)
    centers = rng.uniform(-5, 5, size=(20, args.dim)).astype(np.float32)
    lab = rng.integers(0, 20, size=args.rows)
    data = (centers[lab] + rng.normal(0, 0.35, size=(args.rows, args.dim))).astype(np.float32)
    pks = np.arange(1, args.rows + 1, dtype=np.int64)
    qs = (centers[rng.integers(0, 20, size=40)] +
          rng.normal(0, 0.35, size=(40, args.dim))).astype(np.float32)
    qtext = [lit(q) for q in qs]

    def measure(db, tag):
        rec, lat = [], []
        for qi, q in enumerate(qs):
            t0 = time.perf_counter()
            r = db.req(f"SELECT id, L2_DISTANCE(v, '{qtext[qi]}') AS d FROM docs "
                       f"ORDER BY d LIMIT 10;", "ann")
            lat.append((time.perf_counter() - t0) * 1e6)
            want = set(oracle(data, pks, q))
            got = set(row[0] for row in r["rows"])
            rec.append(len(want & got) / len(want))
        rec = np.array(rec); lat = np.array(lat)
        print(f"{tag}: ann recall mean={rec.mean():.4f} min={rec.min():.4f} "
              f"p50={np.percentile(lat,50):.1f}us p99={np.percentile(lat,99):.1f}us")
        return rec.mean()

    db = DB(binp, d)
    db.req("CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(16), v VECTOR(%d));" % args.dim)
    for s in range(0, args.rows, 200):
        vals = [f"({pks[i]},'c{i%5}','{lit(data[i])}')"
                for i in range(s, min(s + 200, args.rows))]
        db.req("INSERT INTO docs VALUES " + ",".join(vals) + ";")
    db.req("CREATE VECTOR INDEX idx ON docs(v);")
    for qt in qtext[:5]:
        db.req(f"SELECT id, L2_DISTANCE(v, '{qt}') AS d FROM docs ORDER BY d LIMIT 10;", "ann")
    r0 = measure(db, "initial")
    # UPDATE 100 rows and DELETE 50 rows; ann must rebuild/refresh.
    upd = rng.choice(args.rows, size=100, replace=False)
    data[upd] = rng.uniform(-1, 1, size=(100, args.dim)).astype(np.float32)
    for i in upd:
        db.req(f"UPDATE docs SET v = '{lit(data[i])}' WHERE id = {pks[i]};")
    dele = rng.choice(np.setdiff1d(np.arange(args.rows), upd), size=50, replace=False)
    for i in dele:
        db.req(f"DELETE FROM docs WHERE id = {pks[i]};")
    live = np.ones(args.rows, dtype=bool); live[dele] = False
    data_l, pks_l = data[live], pks[live]
    data, pks = data_l, pks_l
    for qt in qtext[:5]:
        db.req(f"SELECT id, L2_DISTANCE(v, '{qt}') AS d FROM docs ORDER BY d LIMIT 10;", "ann")
    r1 = measure(db, "after_dml")
    db.shutdown()
    db = DB(binp, d)
    for qt in qtext[:5]:
        db.req(f"SELECT id, L2_DISTANCE(v, '{qt}') AS d FROM docs ORDER BY d LIMIT 10;", "ann")
    r2 = measure(db, "after_restart")
    db.shutdown()
    shutil.rmtree(d, ignore_errors=True)
    ok = min(r0, r1, r2) >= 0.98
    print("clustered ann checks:", "PASS" if ok else "FAIL")
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
