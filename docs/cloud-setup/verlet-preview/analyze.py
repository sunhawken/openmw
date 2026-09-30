import sys,re,collections,numpy as np
d=collections.defaultdict(list)
for l in open(sys.argv[1]):
    if 'VTRACE' not in l: continue
    p=l.split('VTRACE ')[1].split()
    d[p[0]].append([float(x) for x in p[1:]])
def stats(name):
    a=np.array(d[name]); t=a[:,0]; tip=a[:,1:4]; sp=a[:,4]
    run=(sp>230); walk=(sp>70)&(sp<150)
    out={}
    for lab,m in (('run',run),('walk',walk)):
        idx=np.where(m)[0]
        if len(idx)<8: continue
        # take longest contiguous
        sel=idx[len(idx)//4:]  # steady part
        seg=tip[sel]; tt=t[sel]
        dt=np.diff(tt); ok=dt>0
        # jitter: second difference magnitude normalised by dt^2 is noisy; use deviation from 5-frame moving avg
        k=5
        ma=np.array([seg[max(0,i-k//2):i+k//2+1].mean(0) for i in range(len(seg))])
        dev=np.linalg.norm(seg-ma,axis=1)
        out[lab]=(np.degrees(np.arctan2(np.hypot(seg[:,0],seg[:,1]).mean(),-seg[:,2].mean())), dev.mean(), np.linalg.norm(seg,axis=1).mean(), len(sel))
    return out
names=sorted(d)
for n in names[:3]+names[-2:]:
    print(n,{k:tuple(round(x,2) for x in v) for k,v in stats(n).items()})
allj={'run':[], 'walk':[]}
for n in names:
    for k,v in stats(n).items(): allj[k].append(v)
for k,v in allj.items():
    if v: v=np.array(v); print(k,'mean tilt deg %.1f  jitter(px units) %.3f  len %.1f  n=%d'%(v[:,0].mean(),v[:,1].mean(),v[:,2].mean(),len(v)))
