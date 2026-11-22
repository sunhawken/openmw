#!/usr/bin/env python3
"""Retarget a decoded Source clip to the user's Morrowind skeleton and emit KF 4.0.0.2.

The output is derived from the user's game files. Rotations use global bind-pose
correction; Morrowind bone lengths are preserved.
"""
import argparse
import json
from pathlib import Path
import math
import sys
import numpy as np


def cross(a, b, *args, **kwargs):
    """np.cross for two 3-vectors of doubles: the same six products and three differences."""
    if (not args and not kwargs and type(a) is np.ndarray and type(b) is np.ndarray
            and a.dtype == np.float64 and b.dtype == np.float64 and a.shape == (3,) and b.shape == (3,)):
        ax, ay, az = a.tolist(); bx, by, bz = b.tolist()
        return np.array([ay*bz-az*by, az*bx-ax*bz, ax*by-ay*bx])
    return np.cross(a, b, *args, **kwargs)


def norm(v, *args, **kwargs):
    """np.linalg.norm for a vector of doubles: the square root of its dot product with itself."""
    if not args and not kwargs and type(v) is np.ndarray and v.dtype == np.float64 and v.ndim == 1:
        return np.float64(math.sqrt(v.dot(v)))
    return np.linalg.norm(v, *args, **kwargs)


_MATRICES = {}


def quat_matrix(q):
    # Same reuse as matrix_quat below: identical input bytes, identical result.
    q = np.asarray(q)
    key = (q.dtype.char, q.tobytes())
    m = _MATRICES.get(key)
    if m is None:
        if len(_MATRICES) > 400000: _MATRICES.clear()
        m = _MATRICES[key] = _quat_matrix(q)
    return m.copy()


def _quat_matrix(q):
    x,y,z,w = np.asarray(q)/np.linalg.norm(q)
    return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],
                     [2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],
                     [2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])


_QUATERNIONS = {}


def matrix_quat(m):
    # Fades convert the same start pose for every key; the result for identical
    # input bytes is reused, so nothing changes numerically.
    m=np.asarray(m)
    key=m.tobytes()
    q=_QUATERNIONS.get(key)
    if q is None:
        if len(_QUATERNIONS)>400000:_QUATERNIONS.clear()
        q=_QUATERNIONS[key]=_matrix_quat(m)
    return q.copy()


def prime_quaternions(matrices):
    """Convert many rotation matrices at once into the matrix_quat cache.

    The stacked eigen-decomposition runs the same routine on each 4x4 matrix as a
    single call does, so the cached quaternions are bit-identical.
    """
    pending = {}
    for m in matrices:
        if type(m) is np.ndarray and m.dtype == np.float64 and m.shape == (3, 3):
            key = m.tobytes()
            if key not in _QUATERNIONS:
                pending[key] = m
    if len(pending) < 2:
        return
    if len(_QUATERNIONS)+len(pending) > 400000: _QUATERNIONS.clear()
    m = np.stack(list(pending.values()))
    k = np.empty((len(m), 4, 4))
    k[:, 0, 0] = m[:, 0, 0]-m[:, 1, 1]-m[:, 2, 2]
    k[:, 1, 1] = m[:, 1, 1]-m[:, 0, 0]-m[:, 2, 2]
    k[:, 2, 2] = m[:, 2, 2]-m[:, 0, 0]-m[:, 1, 1]
    k[:, 3, 3] = m[:, 0, 0]+m[:, 1, 1]+m[:, 2, 2]
    k[:, 0, 1] = k[:, 1, 0] = m[:, 1, 0]+m[:, 0, 1]
    k[:, 0, 2] = k[:, 2, 0] = m[:, 2, 0]+m[:, 0, 2]
    k[:, 1, 2] = k[:, 2, 1] = m[:, 2, 1]+m[:, 1, 2]
    k[:, 0, 3] = k[:, 3, 0] = m[:, 2, 1]-m[:, 1, 2]
    k[:, 1, 3] = k[:, 3, 1] = m[:, 0, 2]-m[:, 2, 0]
    k[:, 2, 3] = k[:, 3, 2] = m[:, 1, 0]-m[:, 0, 1]
    _, v = np.linalg.eigh(k/3)
    q = v[:, :, -1][:, [3, 0, 1, 2]]
    q[q[:, 0] < 0] *= -1
    for key, row in zip(pending, q):
        _QUATERNIONS[key] = row.copy()


def _matrix_quat(m):
    # Eigenvector form remains stable near 180 degrees. Return NIF wxyz.
    k=np.array([[m[0,0]-m[1,1]-m[2,2],m[1,0]+m[0,1],m[2,0]+m[0,2],m[2,1]-m[1,2]],
                [m[1,0]+m[0,1],m[1,1]-m[0,0]-m[2,2],m[2,1]+m[1,2],m[0,2]-m[2,0]],
                [m[2,0]+m[0,2],m[2,1]+m[1,2],m[2,2]-m[0,0]-m[1,1],m[1,0]-m[0,1]],
                [m[2,1]-m[1,2],m[0,2]-m[2,0],m[1,0]-m[0,1],np.trace(m)]])/3
    _,v=np.linalg.eigh(k)
    q=v[:,-1][[3,0,1,2]]
    return q if q[0]>=0 else -q


def normalized(name):
    return name.lower().replace('valvebiped.', '').replace('bip01_', 'bip01 ').replace('_',' ')


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('clip',type=Path);ap.add_argument('skeleton',type=Path);ap.add_argument('output',type=Path)
    ap.add_argument('--group',default='brutal_kick');ap.add_argument('--yaw',type=float,default=0)
    ap.add_argument('--hit-time',type=float,default=0.375)
    ap.add_argument('--reference-frame',type=int,help='Use a neutral clip frame rather than MDL bind pose')
    ap.add_argument('--map',type=Path,help='JSON mapping target bone names to Source names; overrides automatic matches')
    ap.add_argument('--es3',type=Path,default=Path(__file__).resolve().parents[2]/'third_party/io_scene_mw/lib')
    args=ap.parse_args();sys.path.insert(0,str(args.es3));from es3 import nif
    source=json.loads(args.clip.read_text());clip=source['clip'];sb=source['bones'];tracks=clip['tracks']
    if clip['flags'] & 4: raise ValueError('Delta animations must first be combined with their reference sequence')
    sk=nif.NiStream();sk.load(args.skeleton)
    nodes=[n for n in sk.objects_of_type(nif.NiNode)];byname={n.name:n for n in nodes}
    parents={c.name:n.name for n in nodes for c in n.children if isinstance(c,nif.NiNode)}
    source_index={normalized(b['name']):i for i,b in enumerate(sb)}
    mapping={n.name:source_index[normalized(n.name)] for n in nodes if normalized(n.name) in source_index}
    if args.map:
        snames={b['name']:i for i,b in enumerate(sb)}
        mapping.update({t:snames[s] for t,s in json.loads(args.map.read_text()).items()})
    if len(mapping)<3:raise ValueError(f'Only {len(mapping)} mapped bones; provide --map')
    def sglobals(local):
        cache={}
        def get(i):
            if i not in cache: cache[i]=(get(sb[i]['parent']) if sb[i]['parent']>=0 else np.eye(3))@local[i]
            return cache[i]
        return [get(i) for i in range(len(sb))]
    reference=[]
    for b in sb:
        q=b['quat_xyzw']
        if args.reference_frame is not None and b['name'] in tracks:
            values=tracks[b['name']]['rotations_xyzw'];q=values[min(args.reference_frame,len(values)-1)]
        reference.append(quat_matrix(q))
    srest=sglobals(reference)
    trest={}
    def rest(n):
        if n not in trest:trest[n]=(rest(parents[n]) if n in parents else np.eye(3))@byname[n].rotation
        return trest[n]
    for n in nodes:rest(n.name)
    theta=np.deg2rad(args.yaw);basis=np.array([[np.cos(theta),-np.sin(theta),0],[np.sin(theta),np.cos(theta),0],[0,0,1]])
    count=clip['frames'];fps=clip['fps'];duration=(count-1)/fps
    keys={name:np.zeros((count,5),np.float32) for name in mapping}
    for frame in range(count):
        slocal=[]
        for b in sb:
            q=tracks.get(b['name'],{}).get('rotations_xyzw',[b['quat_xyzw']])
            slocal.append(quat_matrix(q[min(frame,len(q)-1)]))
        sglobal=sglobals(slocal);tglobal={}
        def animate(name):
            if name not in tglobal:
                parent=animate(parents[name]) if name in parents else np.eye(3)
                if name in mapping:
                    si=mapping[name];world=basis@sglobal[si]@srest[si].T@basis.T@trest[name]
                    local=parent.T@world
                    keys[name][frame]=[frame/fps,*matrix_quat(local)]
                else:world=parent@byname[name].rotation
                tglobal[name]=world
            return tglobal[name]
        for n in nodes:animate(n.name)
    if not 0 <= args.hit_time <= duration:raise ValueError('Hit timestamp is outside animation')
    out=nif.NiStream();text=nif.NiTextKeyExtraData(keys=np.array([(0.,args.group+': start'),(args.hit_time,args.group+': hit'),(duration,args.group+': stop')],dtype='<f,O'))
    out.root=nif.NiSequenceStreamHelper(extra_data=text)
    for name,values in keys.items():
        # Quaternion signs must remain continuous for interpolation.
        for i in range(1,len(values)):
            if np.dot(values[i-1,1:],values[i,1:])<0:values[i,1:]*=-1
        data=nif.NiKeyframeData(rotations=nif.NiRotData(keys=values),translations=nif.NiPosData(keys=np.array([[0,*byname[name].translation]],np.float32)))
        ctrl=nif.NiKeyframeController(data=data,start_time=0.,stop_time=duration,flags=12)
        out.root.extra_datas.append(nif.NiStringExtraData(string_data=name));out.root.controllers.append(ctrl)
    args.output.parent.mkdir(parents=True,exist_ok=True);out.save(args.output)
    check=nif.NiStream();check.load(args.output)
    actual=list(check.root.controllers)
    assert len(actual)==len(keys)
    for ctrl,values in zip(actual,keys.values()):np.testing.assert_allclose(ctrl.data.rotations.keys,values,atol=1e-6)
    print(json.dumps({'output':str(args.output),'group':args.group,'duration':duration,'mapped_bones':list(mapping),'roundtrip':'passed'},indent=2))

if __name__=='__main__':main()
