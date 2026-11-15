import subprocess, json, shutil, os, tempfile, sys, argparse

ap = argparse.ArgumentParser()
ap.add_argument('--bin', default='./build/minivectordb')
A = ap.parse_args(); BIN = A.bin

SETUP = [
    "CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16), n BIGINT);",
    "CREATE TABLE v (id BIGINT PRIMARY KEY, e VECTOR(3));",
]
STMTS = [
    "INSERT INTO t VALUES (1,'a',1),(2,'b',2);",
    "INSERT INTO t (id,s,n) VALUES (3,'c',3);",
    "INSERT INTO t VALUES (4,'it''s',4);",
    "INSERT INTO t VALUES (5,'back\\\\slash',5);",
    "INSERT INTO t VALUES (-6,'neg',-7);",
    "INSERT INTO t VALUES (7,'x',1.0);",
    "INSERT INTO t VALUES (8,'x');",
    "INSERT INTO t VALUES (9,'this-is-way-too-long-for-varchar16',9);",
    "INSERT INTO t VALUES (10,NULL,10);",
    "INSERT INTO t VALUES (11, 'ok', 11);",
    "INSERT INTO t VALUES (12,'ok',12) ;",
    "INSERT INTO t VALUES (13,'ok',13); SELECT id FROM t;",
    "INSERT INTO missing VALUES (1);",
    "INSERT INTO t VALUES (1,'dup',1);",
    "INSERT INTO v VALUES (1,'[1,2,3]'),(2,'[4,5,6]');",
    "INSERT INTO v VALUES (3,'[1,2]');",
    "INSERT INTO v VALUES (4,'[1,2,3,4]');",
    "INSERT INTO v VALUES (5,'[1,2.3]');",
    "INSERT INTO v VALUES (6,'[1,2,3]');",
    "INSERT INTO v VALUES (7,[1,2,3]);",
    "INSERT INTO v VALUES (8,'[1,2,3]');",
    "INSERT INTO t VALUES (14,'ok',14);",
]
VERIFY = [
    "SELECT id,s,n FROM t ORDER BY id;",
    "SELECT id,e FROM v ORDER BY id;",
]

def run(env):
    d = tempfile.mkdtemp(prefix='mvdb-fastins-')
    e = dict(os.environ); e.update(env)
    p = subprocess.Popen([BIN, '--data-dir', d], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=e)
    assert '"ready"' in p.stdout.readline()
    q = [0]
    def req(sql):
        q[0] += 1
        p.stdin.write(json.dumps({'id': q[0], 'op': 'execute', 'sql': sql},
                                 separators=(',', ':')) + '\n')
        p.stdin.flush()
        ln = p.stdout.readline()
        if not ln:
            raise SystemExit('died: ' + p.stderr.read()[:500])
        r = json.loads(ln)
        r.pop('id', None)
        return r
    out = [req(s) for s in SETUP]
    for s in STMTS:
        out.append(req(s))
    out += [req(s) for s in VERIFY]
    p.stdin.write(json.dumps({'id': 9999, 'op': 'shutdown'}) + '\n')
    p.stdin.flush(); p.stdout.readline(); p.wait(timeout=60)
    # restart and verify persistence
    p = subprocess.Popen([BIN, '--data-dir', d], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         text=True, bufsize=1, env=e)
    assert '"ready"' in p.stdout.readline()
    out.append(req(VERIFY[0]))
    out.append(req(VERIFY[1]))
    p.stdin.write(json.dumps({'id': 9998, 'op': 'shutdown'}) + '\n')
    p.stdin.flush(); p.stdout.readline(); p.wait(timeout=60)
    shutil.rmtree(d, ignore_errors=True)
    return out

on = run({})
off = run({'MVDB_NO_FAST_INSERT': '1'})
fails = 0
for i, (a, b) in enumerate(zip(on, off)):
    if a != b:
        fails += 1
        label = (SETUP + STMTS + VERIFY + ['restart: ' + VERIFY[0],
                                           'restart: ' + VERIFY[1]])[i]
        print('DIFF', i, label, '\n on =', a, '\n off=', b)
print('fast insert diff: cases', len(on), 'failures', fails)
sys.exit(0 if fails == 0 else 1)
