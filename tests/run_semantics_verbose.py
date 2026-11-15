import json,os,shutil,subprocess
BIN='./build/minivectordb'; D='/tmp/ev_sem'
def start():
    p=subprocess.Popen([BIN,'--data-dir',D],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1)
    assert '"ready"' in p.stdout.readline(); return p
q=0
def req(p,sql):
    global q; q+=1
    p.stdin.write(json.dumps({'id':q,'op':'execute','sql':sql},separators=(',',':'))+'\n'); p.stdin.flush()
    return json.loads(p.stdout.readline())
shutil.rmtree(D,ignore_errors=True); os.makedirs(D); p=start()
req(p,"CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(16), n BIGINT);")
req(p,"INSERT INTO t VALUES (1,'a',1),(2,'b',2);")
print('noop_int=%d change_int=%d noop_str=%d change_str=%d no_match=%d' % (
 req(p,"UPDATE t SET n=1 WHERE id=1;")['affected_rows'],
 req(p,"UPDATE t SET n=2 WHERE id=1;")['affected_rows'],
 req(p,"UPDATE t SET s='b' WHERE id=2;")['affected_rows'],
 req(p,"UPDATE t SET s='c',n=7 WHERE id=2;")['affected_rows'],
 req(p,"UPDATE t SET n=9 WHERE id=99;")['affected_rows']))
req(p,"BEGIN;"); print('tx_noop=%d' % req(p,"UPDATE t SET n=2 WHERE id=1;")['affected_rows']); req(p,"ROLLBACK;")
print('rows_after_tx=%s' % json.dumps(req(p,"SELECT id,s,n FROM t ORDER BY id;")['rows']))
req(p,"CREATE TABLE vec (id BIGINT PRIMARY KEY, e VECTOR(2));")
req(p,"INSERT INTO vec VALUES (1,'[1,2]'),(2,'[3,4]');")
print('vec_noop=%d vec_change=%d' % (req(p,"UPDATE vec SET e='[1,2]' WHERE id=1;")['affected_rows'], req(p,"UPDATE vec SET e='[2,2]' WHERE id=1;")['affected_rows']))
print('header_tight=%s' % req(p,"SELECT L2_DISTANCE(e,'[1,2]') FROM vec ORDER BY id;")['columns'][0])
print('header_space=%s' % req(p,"SELECT L2_DISTANCE(e, '[1,2]') FROM vec ORDER BY id;")['columns'][0])
print('header_alias=%s' % req(p,"SELECT L2_DISTANCE(e,  '[1,2]' ) AS d FROM vec ORDER BY id;")['columns'][0])
q+=1; p.stdin.write(json.dumps({'id':q,'op':'shutdown'})+'\n'); p.stdin.flush(); p.stdout.readline(); p.wait()
p=start()
print('restart_noop=%d' % req(p,"UPDATE t SET n=2 WHERE id=1;")['affected_rows'])
print('restart_rows=%s' % json.dumps(req(p,"SELECT id,s,n FROM t ORDER BY id;")['rows']))
q+=1; p.stdin.write(json.dumps({'id':q,'op':'shutdown'})+'\n'); p.stdin.flush(); p.stdout.readline(); p.wait()
shutil.rmtree(D,ignore_errors=True)
