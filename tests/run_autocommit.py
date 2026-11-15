import json,os,shutil,subprocess
BIN='./build/minivectordb'; D='/tmp/ev_ac'
def start():
    p=subprocess.Popen([BIN,'--data-dir',D],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    assert '"ready"' in p.stdout.readline(); return p
q=0
def req(p,sql):
    global q; q+=1
    p.stdin.write(json.dumps({'id':q,'op':'execute','sql':sql},separators=(',',':'))+'\n'); p.stdin.flush()
    return json.loads(p.stdout.readline())
def stop(p):
    global q; q+=1; p.stdin.write(json.dumps({'id':q,'op':'shutdown'})+'\n'); p.stdin.flush(); p.stdout.readline(); p.wait()
shutil.rmtree(D,ignore_errors=True); os.makedirs(D); p=start()
req(p,"CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT);")
req(p,"SET autocommit=0;")
req(p,"INSERT INTO t VALUES (1,1);")
req(p,"ROLLBACK;")
print('after_rollback_rows=%s' % json.dumps(req(p,"SELECT id FROM t ORDER BY id;")['rows']))
req(p,"INSERT INTO t VALUES (2,2);")
req(p,"COMMIT;")
print('after_commit_rows=%s' % json.dumps(req(p,"SELECT id FROM t ORDER BY id;")['rows']))
req(p,"SET autocommit=0;")
req(p,"INSERT INTO t VALUES (3,3);")
stop(p)
p=start()
print('restart_after_shutdown_rows=%s' % json.dumps(req(p,"SELECT id FROM t ORDER BY id;")['rows']))
req(p,"SET autocommit=1;")
req(p,"INSERT INTO t VALUES (4,4);")
print('autocommit_on_rows=%s' % json.dumps(req(p,"SELECT id FROM t ORDER BY id;")['rows']))
stop(p); shutil.rmtree(D,ignore_errors=True)
