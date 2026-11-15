import json,os,random,shutil,subprocess,time
BIN=os.environ.get('BIN','./build/minivectordb'); D='/tmp/wal_measure'
shutil.rmtree(D,ignore_errors=True); os.makedirs(D)
rng=random.Random(12345); dim=128; cats=[f"cat{i:02d}" for i in range(50)]
p=subprocess.Popen([BIN,'--data-dir',D],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,bufsize=1,env={**os.environ,'MVDB_PROFILE':'1'})
assert '"ready"' in p.stdout.readline()
q=0
def req(sql):
    global q; q+=1
    p.stdin.write(json.dumps({'id':q,'op':'execute','sql':sql},separators=(',',':'))+'\n'); p.stdin.flush(); return p.stdout.readline()
req(f"CREATE TABLE docs (id BIGINT PRIMARY KEY, category VARCHAR(32), content VARCHAR(1024), embedding VECTOR({dim}));")
t0=time.perf_counter()
for s in range(0,10000,100):
    vals=[]
    for k in range(s,s+100):
        v='['+','.join(f'{rng.uniform(-1,1):.6f}' for _ in range(dim))+']'
        vals.append(f"({k+1},'{cats[k%50]}','document-{k+1:06d}-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx','{v}')")
    req("INSERT INTO docs VALUES "+",".join(vals)+";")
wall=time.perf_counter()-t0
wal=open(os.path.join(D,'wal.log'),'rb').read() if os.path.exists(os.path.join(D,'wal.log')) else b''
print('wall_ms=%.1f wal_bytes=%d' % (wall*1000,len(wal)))
p.stdin.close(); p.wait()
for l in p.stderr.read().splitlines():
    if l.startswith('profile:'): print(l)
shutil.rmtree(D,ignore_errors=True)
