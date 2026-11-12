#!/usr/bin/env python3
"""10k batch insert -> low-threshold checkpoint -> restart -> query regression."""
import argparse, json, os, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'tools'))
import bench as mb

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bin', default='./build/minivectordb')
    ap.add_argument('--rows', type=int, default=10000)
    args = ap.parse_args()
    data = tempfile.mkdtemp(prefix='mvdb_ckpt_data_')
    db = tempfile.mkdtemp(prefix='mvdb_ckpt_db_')
    schema, batches, queries = mb.gen(data, rows=args.rows, dim=128, queries=1)
    def start(extra=()):
        p = subprocess.Popen([args.bin, '--data-dir', db, *extra],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, bufsize=1)
        ready = json.loads(p.stdout.readline())
        assert ready.get('type') == 'ready', ready
        return p
    def send(p, sql):
        p.stdin.write(json.dumps({'id': 1, 'op': 'execute', 'sql': sql}) + '\n')
        p.stdin.flush()
        line = p.stdout.readline()
        assert line, 'database exited before response'
        return json.loads(line)
    p = start(('--checkpoint-bytes', '1'))
    r = send(p, schema); assert r.get('ok'), r
    for b in batches:
        r = send(p, b); assert r.get('ok'), r
    p.stdin.write(json.dumps({'op': 'shutdown'}) + '\n'); p.stdin.flush()
    out = p.stdout.readline(); p.wait()
    err = p.stderr.read()
    assert 'checkpoint failed' not in err, err
    assert p.returncode == 0, (p.returncode, err)
    p = start()
    r = send(p, queries[0]); assert r.get('ok') and len(r.get('rows', [])) == 10, r
    p.stdin.write(json.dumps({'op': 'shutdown'}) + '\n'); p.stdin.flush()
    p.stdout.readline(); p.wait()
    assert p.returncode == 0
    shutil.rmtree(data, ignore_errors=True); shutil.rmtree(db, ignore_errors=True)
    print('checkpoint 10k + restart: PASS')
    return 0

if __name__ == '__main__':
    sys.exit(main())
