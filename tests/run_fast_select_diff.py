import subprocess, json, shutil, os, sys, tempfile
import argparse
ap=argparse.ArgumentParser(); ap.add_argument('--bin',default='./build/minivectordb'); A=ap.parse_args()
BIN=A.bin
SETUP=[
 "CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(8), content VARCHAR(16), embedding VECTOR(4));",
 "INSERT INTO docs VALUES (1,'c0','a','[0,0,0,0]'),(2,'c1','b','[1,0,0,0]'),(3,'c0','c','[0,1,0,0]'),(4,'c2','d','[0,0,1,0]'),(5,'c0','e','[0,0,0,1]');",
]
QUERIES=[
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs WHERE category = 'c0' ORDER BY dist LIMIT 10;",
 "select id, l2_distance(embedding, '[1,0,0,0]') as d from docs order by d limit 2;",
 "SELECT   id ,  L2_DISTANCE( embedding ,  '[0, 0, 0, 0]' )  AS  distance  FROM  docs  ORDER  BY  distance  LIMIT  3 ;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS distance FROM docs ORDER BY distance DESC LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs ORDER BY dist ASC LIMIT 0;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs WHERE category='c1' ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[-1,2e0,-0.5,+1]') AS dist FROM docs ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') FROM docs ORDER BY L2_DISTANCE(embedding, '[0,0,0,0]') LIMIT 10;",
 "SELECT id FROM docs ORDER BY id LIMIT 2;",
 "SELECT id, content FROM docs ORDER BY id LIMIT 2;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs WHERE category = 'c0' AND id > 1 ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs WHERE id = 2 ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[bad]') AS dist FROM docs ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0]') AS dist FROM docs ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs ORDER BY wrong LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs WHERE category = 'c\\'0' ORDER BY dist LIMIT 10;",
 "SELECT id, L2_DISTANCE(embedding, '[0,0,0,0]') AS dist FROM docs ORDER BY dist LIMIT 10; SELECT id FROM docs;",
]
def run(env):
    d=tempfile.mkdtemp(prefix='mvdb-fastdiff-')
    e=dict(os.environ); e.update(env)
    p=subprocess.Popen([BIN,'--data-dir',d],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1,env=e)
    assert '"ready"' in p.stdout.readline()
    q=[0]
    def req(sql, mode='exact'):
        q[0]+=1
        p.stdin.write(json.dumps({'id':q[0],'op':'execute','sql':sql,'search_mode':mode},separators=(',',':'))+'\n'); p.stdin.flush()
        ln=p.stdout.readline()
        if not ln: raise SystemExit('died '+p.stderr.read()[:500])
        return json.loads(ln)
    for s in SETUP: req(s)
    out=[]
    for s in QUERIES:
        try: out.append(req(s))
        except SystemExit as ex: out.append({'died':str(ex)})
    p.stdin.write(json.dumps({'id':9999,'op':'shutdown'})+'\n'); p.stdin.flush(); p.stdout.readline(); p.wait(timeout=60)
    shutil.rmtree(d,ignore_errors=True)
    return out
on=run({}); off=run({'MVDB_NO_FAST_SELECT':'1'})
fails=0
for i,(a,b) in enumerate(zip(on,off)):
    if a!=b:
        fails+=1; print('DIFF',i,QUERIES[i],'\n on=',a,'\n off=',b)
print('fast select diff: cases',len(QUERIES),'failures',fails)
sys.exit(0 if fails==0 else 1)
