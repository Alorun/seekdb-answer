import subprocess,tempfile,shutil,os,json,time,select,collections,sys
ROOT=os.path.dirname(os.path.dirname(os.path.abspath(__file__))); BIN=os.environ.get('MVD_BIN', ROOT+'/build/minivectordb'); CASES=ROOT+'/tests/public_cases/cases'; MARK=b'__MVD_READY__'
class P:
    def __init__(self,d,args):
        self.p=subprocess.Popen([BIN,'--data-dir',d,'--protocol','text','--ready-marker=__MVD_READY__']+args,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
        # Delay deliberately: a startup marker must not shift the first reply.
        time.sleep(0.12)
        self.buf=b''
    def read_until_marker(self,timeout=5):
        lines=[]; end=time.monotonic()+timeout
        while True:
            while b'\n' in self.buf:
                line,self.buf=self.buf.split(b'\n',1)
                if line==MARK: return lines
                lines.append(line.decode('utf-8','replace'))
            left=end-time.monotonic()
            if left<=0: raise TimeoutError('timeout')
            r,_,_=select.select([self.p.stdout],[],[],left)
            if not r: raise TimeoutError('timeout')
            data=os.read(self.p.stdout.fileno(),65536)
            if not data: raise RuntimeError('eof')
            self.buf+=data
    def send(self,s): self.p.stdin.write((s+'\n').encode()); self.p.stdin.flush()
    def close(self):
        try: self.p.stdin.close()
        except: pass
        try: self.p.wait(timeout=5)
        except: self.p.kill(); self.p.wait()
def parse_val(exp,s):
    if isinstance(exp,bool): return s=='true'
    if isinstance(exp,int) and not isinstance(exp,bool): return int(s)
    if isinstance(exp,float): return float(s)
    return s
def same(exp,act):
    if isinstance(exp,float):
        try: return abs(float(act)-exp)<=1e-5*max(1,abs(exp))
        except: return False
    if isinstance(exp,int) and not isinstance(exp,bool):
        try: return int(act)==exp
        except: return False
    return str(act)==str(exp)
def run(name):
    lines=[l for l in open(f'{CASES}/{name}.test',encoding='utf-8').read().splitlines() if l.strip()]
    exp=json.load(open(f'{CASES}/{name}.result',encoding='utf-8')); ei=0; fails=[]; args=[]; d=tempfile.mkdtemp(); p=None
    def restart():
        nonlocal p
        if p: p.close()
        p=P(d,args)
    try:
        p=P(d,args)
        for raw in lines:
            if raw=='--restart': restart(); continue
            if raw.startswith('--mode'):
                if raw.strip()=='--mode ann': args=['--query-mode','ann']
                continue
            if ei>=len(exp): fails.append((raw,'extra')); continue
            e=exp[ei]; ei+=1
            try:
                p.send(raw); got=p.read_until_marker()
            except Exception as ex:
                fails.append((raw,type(ex).__name__,str(ex))); break
            if 'error' in e:
                want='ERROR '+str(e['error'])+':'
                if not got or not got[0].startswith(want): fails.append((raw,'err',want,got[:2]))
                continue
            if 'columns' in e:
                if not got: fails.append((raw,'no header')); continue
                hdr=got[0].split('\t')
                if hdr!=e['columns']: fails.append((raw,'cols',hdr,e['columns']))
                body=got[1:]
                if len(body)!=len(e['rows']): fails.append((raw,'rows count',len(body),len(e['rows']))); continue
                typed=[]
                for er,ar in zip(e['rows'],body):
                    f=ar.split('\t')
                    if len(f)!=len(er): fails.append((raw,'fields',f,er)); typed=[]; break
                    typed.append([parse_val(x,y) for x,y in zip(er,f)])
                if not typed: continue
                if e.get('unordered'):
                    if collections.Counter(map(tuple,e['rows']))!=collections.Counter(map(tuple,typed)): fails.append((raw,'unordered',typed))
                else:
                    for er,ar in zip(e['rows'],typed):
                        if not all(same(x,y) for x,y in zip(er,ar)): fails.append((raw,'data',ar,er)); break
            else:
                if not got or not got[0].startswith('OK'): fails.append((raw,'status',got[:2])); continue
                if 'affected_rows' in e:
                    try: n=int(got[0].split()[1])
                    except: n=None
                    if n!=e['affected_rows']: fails.append((raw,'aff',n,e['affected_rows']))
        if ei!=len(exp): fails.append(('<end>',ei,len(exp)))
    finally:
        if p: p.close()
        shutil.rmtree(d,ignore_errors=True)
    return fails
man=json.load(open(ROOT+'/tests/public_cases/manifest.json')); names=[x['id'] for x in man['cases']]; total=0
for n in names:
    f=run(n); print(('PASS ' if not f else 'FAIL ')+n); [print(' ',x) for x in f[:3]]; total+=len(f)
print('marker cases',len(names),'failures',total); sys.exit(1 if total else 0)
