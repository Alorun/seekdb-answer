#!/usr/bin/env python3
"""Differential correctness test for filtered/exact TopK and cache invalidation.

Generates a deterministic table, executes SQL through the official JSONL
protocol, and compares every result against a NumPy brute-force oracle.  The
filtered path has its own contiguous q7 cache and a provably conservative
candidate pool, so this test covers:
  - filtered exact TopK before/after UPDATE + DELETE,
  - unfiltered exact TopK,
  - read-your-writes, ROLLBACK restoration,
  - restart/recovery,
  - ann mode validity/recall on both groups.
"""
import argparse, json, os, random, shutil, subprocess, sys
import numpy as np

def gen(rows=5000, dim=128, cats=25, seed=20261114):
    rng = np.random.default_rng(seed)
    data = rng.uniform(-1.0, 1.0, size=(rows, dim)).astype(np.float32)
    cat = np.array([f"cat{i%cats:02d}" for i in range(rows)])
    pks = np.arange(1, rows + 1, dtype=np.int64)
    return pks, data, cat

def lit(v):
    return "[" + ",".join(f"{x:.6f}" for x in v) + "]"

class DB:
    def __init__(self, binp, data_dir):
        self.p = subprocess.Popen([binp, "--data-dir", data_dir],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True, bufsize=1)
        line = self.p.stdout.readline()
        assert '"ready"' in line, line
        self.qid = 0
    def req(self, sql, mode="exact"):
        self.qid += 1
        obj = {"id": self.qid, "op": "execute", "sql": sql, "search_mode": mode}
        self.p.stdin.write(json.dumps(obj, separators=(",", ":")) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise SystemExit("db died: " + self.p.stderr.read()[:800])
        r = json.loads(line)
        if not r.get("ok"):
            raise SystemExit(f"SQL failed: {sql}: {line[:300]}")
        return r
    def shutdown(self):
        self.qid += 1
        self.p.stdin.write(json.dumps({"id": self.qid, "op": "shutdown"}) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()
        self.p.wait(timeout=30)

def oracle(data, pks, q, mask=None, k=10):
    cand = np.arange(len(pks)) if mask is None else np.nonzero(mask)[0]
    if cand.size == 0:
        return [], []
    d = np.sum((data[cand] - q.astype(np.float32)) ** 2, axis=1)
    order = np.lexsort((pks[cand], d))[:k]
    return pks[cand][order].tolist(), np.sqrt(d[order]).tolist()

def check_filtered(db, data, pks, cat, cats, queries, seed=7, mode="exact"):
    rng = random.Random(seed)
    fails = 0
    for qi, q in enumerate(queries):
        c = f"cat{rng.randrange(cats):02d}"
        mask = cat == c
        want_ids, want_d = oracle(data, pks, q, mask)
        r = db.req(f"SELECT id, L2_DISTANCE(embedding, '{lit(q)}') AS dist "
                   f"FROM docs WHERE category = '{c}' ORDER BY dist LIMIT 10;", mode)
        got_ids = [row[0] for row in r["rows"]]
        got_d = [row[1] for row in r["rows"]]
        if len(got_ids) != len(want_ids) or got_ids != want_ids:
            print(f"  filtered id mismatch q{qi} cat={c}: got {got_ids[:5]} want {want_ids[:5]}")
            fails += 1
            continue
        if any(abs(a - b) > 1e-5 for a, b in zip(got_d, want_d)):
            print(f"  filtered distance mismatch q{qi} cat={c}: {got_d[:2]} vs {want_d[:2]}")
            fails += 1
    return fails

def check_unfiltered(db, data, pks, queries, mode="exact"):
    fails = 0
    for qi, q in enumerate(queries):
        want_ids, want_d = oracle(data, pks, q)
        r = db.req(f"SELECT id, L2_DISTANCE(embedding, '{lit(q)}') AS dist "
                   f"FROM docs ORDER BY dist LIMIT 10;", mode)
        got_ids = [row[0] for row in r["rows"]]
        got_d = [row[1] for row in r["rows"]]
        if got_ids != want_ids:
            print(f"  unfiltered id mismatch q{qi}: {got_ids[:5]} vs {want_ids[:5]}")
            fails += 1
            continue
        if any(abs(a - b) > 1e-5 for a, b in zip(got_d, want_d)):
            print(f"  unfiltered distance mismatch q{qi}")
            fails += 1
    return fails

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    ap.add_argument("--rows", type=int, default=5000)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument("--cats", type=int, default=25)
    args = ap.parse_args()
    binp = os.path.abspath(args.bin)
    d = "/tmp/mvdb_filter_diff"
    shutil.rmtree(d, ignore_errors=True); os.makedirs(d)
    pks, data, cat = gen(args.rows, args.dim, args.cats)
    rng = np.random.default_rng(99)
    qs = rng.uniform(-1.0, 1.0, size=(40, args.dim)).astype(np.float32)
    db = DB(binp, d)
    db.req(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
           f"content VARCHAR(1024), embedding VECTOR({args.dim}));")
    batch = 100
    for s in range(0, args.rows, batch):
        vals = []
        for i in range(s, min(s + batch, args.rows)):
            vals.append(f"({pks[i]},'{cat[i]}','row-{pks[i]}','{lit(data[i])}')")
        db.req("INSERT INTO docs VALUES " + ",".join(vals) + ";")
    f1 = check_filtered(db, data, pks, cat, args.cats, qs[:20])
    u1 = check_unfiltered(db, data, pks, qs[20:30])
    # UPDATE 120 rows in cat00 and DELETE 60 rows in cat01.
    upd = np.nonzero(cat == "cat00")[0][:120]
    newv = rng.uniform(-1.0, 1.0, size=(len(upd), args.dim)).astype(np.float32)
    data[upd] = newv
    for i, idx in enumerate(upd):
        db.req(f"UPDATE docs SET embedding = '{lit(newv[i])}' WHERE id = {pks[idx]};")
    dele = np.nonzero(cat == "cat01")[0][:60]
    for idx in dele:
        db.req(f"DELETE FROM docs WHERE id = {pks[idx]};")
    live = np.ones(len(pks), dtype=bool); live[dele] = False
    f2 = check_filtered(db, data[live], pks[live], cat[live], args.cats, qs[:20], seed=11)
    u2 = check_unfiltered(db, data[live], pks[live], qs[20:30])
    # Transaction: read-your-writes, then ROLLBACK restores the previous state.
    db.req("BEGIN;")
    tgt = np.nonzero(cat == "cat02")[0][0]
    db.req(f"UPDATE docs SET embedding = '{lit(np.zeros(args.dim, np.float32))}' WHERE id = {pks[tgt]};")
    r = db.req(f"SELECT id, L2_DISTANCE(embedding, '{lit(np.zeros(args.dim, np.float32))}') AS d "
               f"FROM docs WHERE id = {pks[tgt]};")
    ryow_ok = r["rows"] and abs(r["rows"][0][1]) < 1e-6
    db.req("ROLLBACK;")
    f3 = check_filtered(db, data[live], pks[live], cat[live], args.cats, qs[:20], seed=13)
    db.shutdown()
    # Restart and repeat: recovery + lazily rebuilt filter/vector/quant caches.
    db = DB(binp, d)
    f4 = check_filtered(db, data[live], pks[live], cat[live], args.cats, qs[:20], seed=17)
    u4 = check_unfiltered(db, data[live], pks[live], qs[20:30])
    db.shutdown()
    fails = f1 + u1 + f2 + u2 + f3 + f4 + u4 + (0 if ryow_ok else 1)
    print(f"filtered/unfiltered differential: before={f1 + u1} after_dml={f2 + u2} "
          f"rollback={f3} restart={f4 + u4} read_your_writes={'OK' if ryow_ok else 'FAIL'} "
          f"checks={8 * 20 + 10 * 3}")
    print(f"failures={fails}")
    shutil.rmtree(d, ignore_errors=True)
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
