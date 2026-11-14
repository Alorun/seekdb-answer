#!/usr/bin/env python3
"""Reproducible local profile: client wall vs server phases, write vs query.

Populates 10k x 128 rows over JSONL, restarts, then measures:
  * pk-only floor (SELECT id LIMIT 10, no vector)
  * filtered TopK (500 queries)
  * unfiltered TopK (500 queries)
  * write phase split: client json.dumps, pipe write, response wait, server
    MVDB_PROFILE parse/exec/format (profile printed after stdin EOF)
No numpy, no cgroup access; safe for any machine.
"""
import argparse, json, os, random, shutil, subprocess, time

def gen(rows=10000, dim=128, batch=100, cats=50, seed=12345):
    rng = random.Random(seed)
    cn = [f"cat{i:02d}" for i in range(cats)]
    schema = (f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
              f"content VARCHAR(1024), embedding VECTOR({dim}));")
    batches = []
    row = 0
    while row < rows:
        n = min(batch, rows - row)
        tup = []
        for k in range(n):
            i = row + k + 1
            v = "[" + ",".join(f"{rng.uniform(-1, 1):.6f}" for _ in range(dim)) + "]"
            tup.append(f"({i},'{cn[i%cats]}','document-{i:06d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx','{v}')")
        batches.append("INSERT INTO docs VALUES " + ",".join(tup) + ";")
        row += n
    qs = []
    for q in range(1000):
        v = "[" + ",".join(f"{rng.uniform(-1, 1):.6f}" for _ in range(dim)) + "]"
        if q < 500:
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs WHERE category = '{cn[q%cats]}' ORDER BY dist LIMIT 10;")
        else:
            qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs ORDER BY dist LIMIT 10;")
    return schema, batches, qs

def line(i, sql, mode="exact"):
    return json.dumps({"id": i, "op": "execute", "sql": sql, "search_mode": mode},
                      separators=(",", ":")) + "\n"

def start(binp, data_dir):
    env = dict(os.environ); env["MVDB_PROFILE"] = "1"
    p = subprocess.Popen([binp, "--data-dir", data_dir], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=env)
    assert '"ready"' in p.stdout.readline()
    return p

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/minivectordb")
    ap.add_argument("--dir", default="/tmp/mvdb_profile")
    args = ap.parse_args()
    shutil.rmtree(args.dir, ignore_errors=True); os.makedirs(args.dir)
    schema, batches, qs = gen()
    binp = os.path.abspath(args.bin)
    p = start(binp, args.dir)
    qid = [0]
    def req(sql, mode="exact"):
        qid[0] += 1
        p.stdin.write(line(qid[0], sql, mode)); p.stdin.flush()
        ln = p.stdout.readline()
        if not ln: raise SystemExit("db died: " + p.stderr.read()[:400])
        return ln
    t0 = time.perf_counter(); req(schema)
    for b in batches: req(b)
    write_wall = time.perf_counter() - t0
    p.stdin.close(); p.wait()
    prof = [l for l in p.stderr.read().splitlines() if l.startswith("profile:")]
    print(f"write_wall_ms={write_wall*1000:.1f}")
    print(prof[0] if prof else "write_profile=missing")
    # restart for query-only profiling
    p = start(binp, args.dir)
    qid[0] = 0
    def q(sql, mode="exact"):
        qid[0] += 1
        p.stdin.write(line(qid[0], sql, mode)); p.stdin.flush()
        p.stdout.readline()
    for i in range(50): q(qs[i])
    for i in range(500, 550): q(qs[i])
    def group(sel):
        t0 = time.perf_counter()
        for x in sel: q(x)
        return (time.perf_counter() - t0) * 1e6 / len(sel)
    filt = group(qs[:500])
    unf = group(qs[500:])
    pk = group(["SELECT id FROM docs ORDER BY id LIMIT 10;"] * 500)
    p.stdin.close(); p.wait()
    prof = [l for l in p.stderr.read().splitlines() if l.startswith("profile:")]
    print(f"pk_floor_us={pk:.1f} filtered_us={filt:.1f} unfiltered_us={unf:.1f}")
    print(prof[0] if prof else "query_profile=missing")
    shutil.rmtree(args.dir, ignore_errors=True)

if __name__ == "__main__":
    main()
