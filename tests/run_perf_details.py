#!/usr/bin/env python3
"""A/B microbenchmark for the three S6 details.

mode=query  fast SELECT on/off on one binary (MVDB_NO_FAST_SELECT)
mode=write  direct vector literal into the Value buffer on/off
mode=q7     two binaries (before/after the 64-byte code alignment), fast path off

All runs use the same 10k x 128 uniform workload, 4 reps, and report the median
wall/parse/exec from MVDB_PROFILE.
"""
import argparse, json, os, shutil, subprocess, tempfile, time, statistics
import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('--mode', choices=['query', 'write', 'q7'], required=True)
ap.add_argument('--bin-a', default='./build/minivectordb')
ap.add_argument('--bin-b', default='./build/minivectordb')
ap.add_argument('--reps', type=int, default=4)
A = ap.parse_args()
BIN_A = os.path.abspath(A.bin_a); BIN_B = os.path.abspath(A.bin_b)
RNG = np.random.default_rng(2026); N = 10000; DIM = 128
DATA = RNG.uniform(-1, 1, size=(N, DIM)).astype(np.float32)
PKS = np.arange(1, N + 1)
QS = RNG.uniform(-1, 1, size=(500, DIM)).astype(np.float32)

def lit(v):
    return '[' + ','.join(f'{x:.6f}' for x in v) + ']'

QTEXT = [lit(q) for q in QS]
QUERIES = []
for i in range(500):
    q = QTEXT[i]
    QUERIES.append(f"SELECT id, L2_DISTANCE(embedding, '{q}') AS dist FROM docs WHERE category = 'c{i%5}' ORDER BY dist LIMIT 10;")
    QUERIES.append(f"SELECT id, L2_DISTANCE(embedding, '{q}') AS dist FROM docs ORDER BY dist LIMIT 10;")
BATCHES = []
for s in range(0, N, 100):
    vals = [f"({int(PKS[i])},'c{i%5}','x','{lit(DATA[i])}')" for i in range(s, min(s + 100, N))]
    BATCHES.append('INSERT INTO docs VALUES ' + ','.join(vals) + ';')

def start(binp, data_dir, env=None):
    e = dict(os.environ); e.update(env or {}); e['MVDB_PROFILE'] = '1'
    p = subprocess.Popen([binp, '--data-dir', data_dir], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=e)
    assert '"ready"' in p.stdout.readline(), 'server did not become ready'
    return p

def req(p, i, sql):
    p.stdin.write(json.dumps({'id': i, 'op': 'execute', 'sql': sql,
                              'search_mode': 'exact'}, separators=(',', ':')) + '\n')
    p.stdin.flush()
    line = p.stdout.readline()
    if not line:
        raise SystemExit('server died: ' + p.stderr.read()[:500])
    return json.loads(line)

def profile(p, requests):
    p.stdin.close(); p.wait(timeout=180)
    lines = [l for l in p.stderr.read().splitlines() if l.startswith('profile:')]
    if not lines:
        return {}
    return {k: float(v) * 1e6 / requests for k, v in (x.split('=') for x in lines[0].split()[1:])}

def make_data(binp):
    d = tempfile.mkdtemp(prefix='mvdb-perf-')
    p = start(binp, d)
    req(p, 1, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(128));")
    for i, b in enumerate(BATCHES, 2):
        req(p, i, b)
    p.stdin.close(); p.wait(timeout=180)
    return d

def run_queries(binp, data_dir, env):
    p = start(binp, data_dir, env)
    for i in range(20):
        req(p, 1000 + i, QUERIES[i % len(QUERIES)])
    t0 = time.perf_counter()
    for i in range(1000):
        req(p, 2000 + i, QUERIES[i])
    wall = (time.perf_counter() - t0) * 1e6 / 1000
    return wall, profile(p, 1020)

def run_write(binp, env):
    d = tempfile.mkdtemp(prefix='mvdb-perf-w-')
    p = start(binp, d, env)
    req(p, 1, "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(128));")
    t0 = time.perf_counter()
    for i, b in enumerate(BATCHES, 2):
        req(p, i, b)
    wall = (time.perf_counter() - t0) * 1000
    prof = profile(p, 101)
    shutil.rmtree(d, ignore_errors=True)
    return wall, prof

def med(vals):
    return statistics.median(vals)

if A.mode == 'query':
    d = make_data(BIN_B)
    res = {}
    for tag, env in [('on', {}), ('off', {'MVDB_NO_FAST_SELECT': '1'})]:
        rows = [run_queries(BIN_B, d, env) for _ in range(A.reps)]
        res[tag] = rows
        for wall, prof in rows:
            print(f'mode=query {tag} wall={wall:.1f} parse={prof.get("parse",0):.2f} exec={prof.get("exec",0):.2f}')
    for tag in res:
        print(f'median query {tag} wall={med([x[0] for x in res[tag]]):.1f} parse={med([x[1].get("parse",0) for x in res[tag]]):.2f} exec={med([x[1].get("exec",0) for x in res[tag]]):.2f}')
    shutil.rmtree(d, ignore_errors=True)
elif A.mode == 'write':
    res = {}
    for tag, env in [('direct', {}), ('old', {'MVDB_NO_DIRECT_VEC': '1'})]:
        rows = [run_write(BIN_B, env) for _ in range(A.reps)]
        res[tag] = rows
        for wall, prof in rows:
            print(f'mode=write {tag} wall={wall:.1f} parse={prof.get("parse",0):.1f} exec={prof.get("exec",0):.1f}')
    for tag in res:
        print(f'median write {tag} wall={med([x[0] for x in res[tag]]):.1f} parse={med([x[1].get("parse",0) for x in res[tag]]):.1f} exec={med([x[1].get("exec",0) for x in res[tag]]):.1f}')
else:
    d = make_data(BIN_B)
    res = {}
    for tag, binp in [('a', BIN_A), ('b', BIN_B)]:
        rows = [run_queries(binp, d, {'MVDB_NO_FAST_SELECT': '1'}) for _ in range(A.reps)]
        res[tag] = rows
        for wall, prof in rows:
            print(f'mode=q7 {tag} wall={wall:.1f} parse={prof.get("parse",0):.2f} exec={prof.get("exec",0):.2f}')
    for tag in res:
        print(f'median q7 {tag} wall={med([x[0] for x in res[tag]]):.1f} parse={med([x[1].get("parse",0) for x in res[tag]]):.2f} exec={med([x[1].get("exec",0) for x in res[tag]]):.2f}')
    shutil.rmtree(d, ignore_errors=True)
