#!/usr/bin/env python3
"""Read cgroup v2 memory.stat anon/file and process RssAnon/RssFile per phase.

Used as learning evidence for the M metric: `memory.peak` is the whole
container, including page cache, not just process RSS.
"""
import argparse, json, os, random, shutil, subprocess, time

def gen(rows=10000, dim=128, batch=100, seed=12345):
    rng = random.Random(seed); cn = [f"cat{i:02d}" for i in range(50)]
    schema = (f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), "
              f"content VARCHAR(1024), embedding VECTOR({dim}));")
    batches = []; row = 0
    while row < rows:
        n = min(batch, rows - row); tup = []
        for k in range(n):
            i = row + k + 1
            v = "[" + ",".join(f"{rng.uniform(-1,1):.6f}" for _ in range(dim)) + "]"
            tup.append(f"({i},'{cn[i%50]}','document-{i:06d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx','{v}')")
        batches.append("INSERT INTO docs VALUES " + ",".join(tup) + ";"); row += n
    qs = []
    for q in range(40):
        v = "[" + ",".join(f"{rng.uniform(-1,1):.6f}" for _ in range(dim)) + "]"
        qs.append(f"SELECT id, L2_DISTANCE(embedding, '{v}') AS dist FROM docs ORDER BY dist LIMIT 10;")
    return schema, batches, qs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", default="mvdb:work")
    ap.add_argument("--rows", type=int, default=10000)
    ap.add_argument("--rows-disk", type=int, default=50000)
    ap.add_argument("--cpus", default="2")
    ap.add_argument("--mem", default="2g")
    args = ap.parse_args()
    schema, batches, qs = gen(args.rows)
    vol = f"mvdb_mem_vol_{os.getpid()}"
    subprocess.run(["docker", "volume", "rm", "-f", vol], capture_output=True)
    subprocess.run(["docker", "volume", "create", vol], capture_output=True)
    name = f"mvdb_mem_{os.getpid()}"
    p = subprocess.Popen(["docker", "run", "-i", "--name", name, "--cpus", args.cpus,
                          "--memory", args.mem, "-v", f"{vol}:/data", args.image],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, bufsize=1)
    assert '"ready"' in p.stdout.readline()
    full = subprocess.check_output(["docker", "inspect", "-f", "{{.Id}}", name], text=True).strip()
    base = f"/sys/fs/cgroup/system.slice/docker-{full}.scope"
    pid = subprocess.check_output(["docker", "inspect", "-f", "{{.State.Pid}}", name], text=True).strip()
    def mem(phase):
        st = {}
        try:
            for ln in open(os.path.join(base, "memory.stat")):
                k, v = ln.split()
                st[k] = int(v)
        except Exception:
            pass
        peak = 0
        try: peak = int(open(os.path.join(base, "memory.peak")).read())
        except Exception: pass
        rss = {"RssAnon": 0, "RssFile": 0}
        try:
            for ln in open(f"/proc/{pid}/status"):
                if ln.startswith(("RssAnon:", "RssFile:")):
                    k, v = ln.split(); rss[k[:-1]] = int(v) * 1024
        except Exception:
            pass
        print(f"mem phase={phase} peak={peak/1e6:.1f}MB anon={st.get('anon',0)/1e6:.1f}MB "
              f"file={st.get('file',0)/1e6:.1f}MB rss_anon={rss['RssAnon']/1e6:.1f}MB "
              f"rss_file={rss['RssFile']/1e6:.1f}MB")
    q = 0
    def req(sql):
        nonlocal q; q += 1
        p.stdin.write(json.dumps({"id": q, "op": "execute", "sql": sql}, separators=(",", ":")) + "\n")
        p.stdin.flush(); return p.stdout.readline()
    req(schema)
    for b in batches: req(b)
    time.sleep(0.2); mem("write")
    for s in qs: req(s)
    time.sleep(0.2); mem("query")
    p.stdin.write(json.dumps({"id": q + 1, "op": "shutdown"}) + "\n"); p.stdin.flush()
    p.stdout.readline(); p.wait(timeout=120)
    subprocess.run(["docker", "rm", "-f", name], capture_output=True)
    subprocess.run(["docker", "volume", "rm", "-f", vol], capture_output=True)

if __name__ == "__main__":
    main()
