import numpy as np
rng=np.random.default_rng(1); N,D,K=10000,128,10
X=rng.uniform(-1,1,(N,D)).astype(np.float32); Q=rng.uniform(-1,1,(100,D)).astype(np.float32)
def run(bits):
    levels=(1<<(bits-1))-1; scale=np.abs(X).max()/levels
    C=np.rint(X/scale).clip(-levels,levels).astype(np.int32)
    err=np.sqrt(((X-C.astype(np.float32)*scale)**2).sum(1))
    fr=[]
    for q in Q:
        cq=np.rint(q/scale).clip(-levels,levels).astype(np.int32)
        dq=((C-cq)**2).sum(1).astype(np.float64); dk=np.partition(dq,K-1)[K-1]
        E=err.max()+np.sqrt(((q-cq.astype(np.float32)*scale)**2).sum())
        thr=(np.sqrt(max(0,dk))+2*E/scale)**2*(1+1e-6)+1e-12
        fr.append((dq<=thr).mean())
    print(f'bits={bits} scale={scale:.4f} max_err={err.max():.3f} cand_mean={np.mean(fr):.4f} cand_p95={np.percentile(fr,95):.4f}')
for b in [7,6,5,4]: run(b)
