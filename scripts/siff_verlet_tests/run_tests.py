import ctypes as C, json, math
from pathlib import Path
import numpy as np
from scipy.spatial.transform import Rotation

ROOT=Path(__file__).resolve().parent
def matrix(t=(0,0,0), angles=(0,0,0)):
    m=np.eye(4);m[:3,:3]=Rotation.from_euler('xyz',angles).as_matrix().T;m[3,:3]=t;return m

class Asset:
    def __init__(self,name):
        self.name=name;self.d=dict(np.load(ROOT/(name+'.npz')));self.meta=json.loads((ROOT/(name+'.json')).read_text());self.names=self.meta['names'];self.chain=self.d['chain'];self.n=len(self.names)
        self.rest=self.d['world'][self.chain,3,:3].copy()
    def skin(self,world,which=0):
        d=self.d;i=which;bi=d[f'bi{i}'];skin=d[f'skin{i}'];bind=d[f'bind{i}'];v=d[f'v{i}'];w=d[f'w{i}'];root=self.meta['shapes'][i]['root']
        tm=bind@world[bi]@np.linalg.inv(world[root])@skin
        blend=np.einsum('vb,bij->vij',w,tm,optimize=True)
        return np.einsum('vi,vij->vj',np.column_stack([v,np.ones(len(v))]),blend,optimize=True)[:,:3]

class Rig:
    def __init__(self,asset,libname,settings=None):
        self.a=asset;self.lib=C.CDLL(str(ROOT/libname));l=self.lib
        l.rig_create.restype=C.c_void_p
        l.rig_create.argtypes=[C.c_int,C.c_void_p,C.c_void_p,C.c_void_p,C.c_int,C.c_void_p,C.c_void_p,C.c_void_p]
        l.rig_tick.argtypes=[C.c_void_p,C.c_double];l.rig_set_matrix.argtypes=[C.c_void_p,C.c_int,C.c_void_p]
        for f in ['rig_positions','rig_previous','rig_world']:getattr(l,f).argtypes=[C.c_void_p,C.c_void_p]
        l.rig_impulse.argtypes=[C.c_void_p,C.c_float,C.c_float,C.c_float];l.rig_destroy.argtypes=[C.c_void_p]
        names=(C.c_char_p*asset.n)(*[x.encode() for x in asset.names]);d=asset.d
        starts=np.arange(0,asset.chain.size+1,asset.chain.shape[1],dtype=np.int32)
        st=settings or ([4,.5,.94,6,1,.38] if 'hair' in asset.name else [3,.42,.91,6,.95,.24])
        if len(st)==6:st=list(st)+[.30 if 'hair' in asset.name else .22,1800]
        if len(st)==8:st=list(st)+[3.5 if 'hair' in asset.name else 2.5]
        if len(st)==9:st=list(st)+[0,1500]
        if len(st)==11:st=list(st)+[0]
        st=np.array(st,dtype=np.float32)
        self.ptr=l.rig_create(asset.n,d['parents'].ctypes.data,C.cast(names,C.c_void_p),d['local'].ctypes.data,12,starts.ctypes.data,asset.chain.ctypes.data,st.ctypes.data)
        self.positions=np.empty((12,asset.chain.shape[1],3),np.float32);self.previous=np.empty_like(self.positions);self.world=np.empty_like(d['world'])
    def set(self,i,m):self.lib.rig_set_matrix(self.ptr,i,np.ascontiguousarray(m,np.float64).ctypes.data)
    def tick(self,t,world=False):
        self.lib.rig_tick(self.ptr,t);self.lib.rig_positions(self.ptr,self.positions.ctypes.data);self.lib.rig_previous(self.ptr,self.previous.ctypes.data)
        if world:self.lib.rig_world(self.ptr,self.world.ctypes.data)
    def close(self):self.lib.rig_destroy(self.ptr)

def sequence(kind,direction=0,fps=60,duration=6):
    # Actor faces +Y; 180 degrees is backwards, independent of facing yaw.
    u=np.array([math.sin(direction),math.cos(direction),0.])
    if kind in ['walk','run','jump']:
        speed=110 if kind=='walk' else 320
        for f in range(int(duration*fps)+1):
            t=f/fps;start=np.clip(t/.3,0,1);stop=np.clip((4.0-t)/.3,0,1);v=speed*start*stop
            # Position integral of smooth start/cruise/stop via explicit small steps.
            if f==0:p=np.zeros(3)
            else:p=p+u*v/fps
            if kind=='jump':
                q=(t-1.4);p[2]=max(0,360*q-.5*711*q*q) if q>=0 else 0
            phase=t*(7 if kind=='walk' else 11)
            bob=(.55 if kind=='walk' else 1.2)*math.sin(2*phase)*start*stop
            yield t,matrix(p+np.array([0,0,bob]),(0,0,0)),phase,start*stop
    elif kind=='pull':
        for f in range(int(duration*fps)+1):
            t=f/fps;yield t,matrix(),t*5,0
    elif kind=='extreme':
        for f in range(int(duration*fps)+1):
            t=f/fps
            if t<1:p=np.array([0.,0.,0.]);angles=(0,0,0)
            elif t<3.5:
                p=np.array([27*math.sin(t*12),32*math.sin(t*15),max(0,70*math.sin(t*5))]);angles=(.30*math.sin(t*9),.22*math.cos(t*7),math.pi*int(t*7))
            else:p=np.zeros(3);angles=(0,0,0)
            yield t,matrix(p,angles),t*13,float(1<t<3.5)

def animate(r,phase,amount):
    # Procedural opposing leg/head motion around the actual NIF bind skeleton.
    # This tests collision and attachments; it is not an exported game animation.
    for name,axis,value in [('Bip01 L Thigh',0,.25*math.sin(phase)*amount),('Bip01 R Thigh',0,-.25*math.sin(phase)*amount),('Bip01 L Calf',0,.35*max(0,-math.sin(phase))*amount),('Bip01 R Calf',0,.35*max(0,math.sin(phase))*amount),('Bip01 Head',2,.05*math.sin(phase*.5)*amount)]:
        if name in r.a.names:
            idx=r.a.names.index(name);m=r.a.d['local'][idx].copy();ang=[0.,0.,0.];ang[axis]=value
            parent=r.a.d['parents'][idx];pr=r.a.d['world'][parent,:3,:3]
            m[:3,:3]=m[:3,:3]@pr@matrix(angles=ang)[:3,:3]@np.linalg.inv(pr)
            r.set(idx,m)

def run(asset,lib,kind,angle=0,fps=60,record=False,settings=None,pull_vector=None,duration=None):
    r=Rig(asset,lib,settings);history=[];worlds=[];restlen=np.linalg.norm(np.diff(asset.rest,axis=1),axis=2)
    worst=1.;thickness=1.;free_thickness=1.;maxvel=0.;last=[]
    initial=asset.rest
    ringbase=[]
    for i in range(3,initial.shape[1]):
        centered=initial[:,i]-initial[:,i].mean(0);ringbase.append(np.linalg.svd(centered,compute_uv=False)[1])
    duration=duration or (9 if kind=='extreme' else 6)
    for frame,(t,m,phase,amount) in enumerate(sequence(kind,angle,fps,duration)):
        r.set(0,m);animate(r,phase,amount)
        if kind=='pull' and frame==int(fps):
            u=np.array(pull_vector if pull_vector is not None else [math.sin(angle),math.cos(angle),.45],float);u/=np.linalg.norm(u);r.lib.rig_impulse(r.ptr,*[float(x*150/(fps*6)) for x in u])
        capture=record and frame%max(1,int(fps/24))==0
        r.tick(t,world=capture);p=r.positions.astype(float)
        if not np.isfinite(p).all():raise RuntimeError('non-finite positions')
        length=np.linalg.norm(np.diff(p,axis=1),axis=2);worst=max(worst,float((length[:,2:]/restlen[:,2:]).max()))
        # Singular values detect flat planes irrespective of facing rotation.
        for k,i in enumerate(range(3,p.shape[1])):
            s=np.linalg.svd(p[:,i]-p[:,i].mean(0),compute_uv=False)
            thickness=min(thickness,float(s[1]/ringbase[k]))
            # Rings actively folded against the floor are draping contacts.
            # Keep their metric, but assess unsupported shell volume separately.
            if p[:,i,2].min() > m[3,2]+.60:
                free_thickness=min(free_thickness,float(s[1]/ringbase[k]))
        h=min(1/fps,.05)/max(6,math.ceil(min(1/fps,.05)*240))
        maxvel=max(maxvel,float(np.linalg.norm(r.positions-r.previous,axis=2).max())/h)
        if frame>(duration-1)*fps:
            local=np.column_stack([p.reshape(-1,3),np.ones(p.shape[0]*p.shape[1])])@np.linalg.inv(m)
            last.append(local[:,:3])
        if capture:history.append(p.copy());worlds.append(r.world.copy())
    settle=0 if len(last)<2 else float(np.sqrt(np.mean(np.diff(last,axis=0)**2))*fps)
    r.close()
    result={'case':kind,'direction_deg':round(math.degrees(angle)),'fps':fps,'max_segment_ratio':worst,'min_ring_thickness_ratio':thickness,'min_unsupported_ring_thickness_ratio':free_thickness,'max_hidden_speed':maxvel,'settle_rms_units_per_s':settle}
    if record:result.update(history=np.array(history),worlds=np.array(worlds))
    return result

def main():
    report={}
    for name in ['siff_hair','siff_regalia']:
        a=Asset(name)
        errs=[float(np.abs(a.skin(a.d['world'],i)-a.d[f'ref{i}']).max()) for i in range(len(a.meta['shapes']))]
        print(name,'skin evaluator error',max(errs),'rest extrema',a.rest.min((0,1)),a.rest.max((0,1)),flush=True)
        cases=[]
        for kind in ['walk','run','jump','pull']:
            for deg in range(0,360,45):cases.append(run(a,'baseline.so',kind,math.radians(deg)))
        cases.append(run(a,'baseline.so','extreme'))
        report[name]=cases
        print(name,'cases',len(cases),'worst',max(x['max_segment_ratio'] for x in cases),'thickness',min(x['min_ring_thickness_ratio'] for x in cases),'hidden speed',max(x['max_hidden_speed'] for x in cases),flush=True)
    (ROOT/'baseline-results.json').write_text(json.dumps(report,indent=2))

if __name__=='__main__':main()
