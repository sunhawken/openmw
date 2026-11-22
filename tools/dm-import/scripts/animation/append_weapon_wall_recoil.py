#!/usr/bin/env python3
"""Append wall deflections to the staff and dagger first-person sets.

Staff and daggers have no wall-bounce clip of their own. Their parry deflections
supply a short, correctly held reaction; the engine adds contact-plane withdrawal
to both arm chains. Existing playable keys and dagger item primitives keep their
exact values and timestamps.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import numpy as np
from audit_viewmodel_continuity import nif


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def load(p):
    s=nif.NiStream();s.load(p);return s


def extra(s):
    return next(e for e in s.root.extra_datas if isinstance(e,nif.NiTextKeyExtraData))


def controllers(s):
    return dict(zip([e.string_data for e in s.root.extra_datas if isinstance(e,nif.NiStringExtraData)],s.root.controllers))


def apply(data,family,manifest):
    folder=data/'Animations'/('brutal_fp_'+family)
    path=folder/'weapononehand.kf';s=load(path);ex=extra(s)
    oldmarks=[(float(t),str(k))for t,k in ex.keys]
    if any(': bounce' in k for _,k in oldmarks):raise ValueError('Wall recoil already present: '+family)
    channels=controllers(s)
    # Append beyond all existing keys, including the non-playable item table.
    # Explicit bounce start/stop selects only these sections. The global
    # stop stays fixed so ordinary gameplay can never traverse the primitive table.
    cursor=max(float(t)for c in channels.values()for field in ('rotations','translations','scales')for t in getattr(c.data,field).keys[:,0])+.1
    marks=list(oldmarks);report=[];before={}
    for name,c in channels.items():
        for field in ('rotations','translations','scales'):before[name,field]=getattr(c.data,field).keys.copy()
    for key,index in [('bounce',1),('bouncel',2),('bouncer',3)]:
        group='brutal_blockhit'+str(index);sourcepath=folder/(group+'.kf');source=load(sourcepath)
        sourcekeys=[(float(t),str(k))for t,k in extra(source).keys]
        start=next(t for t,k in sourcekeys if k==group+': start')
        stop=next(t for t,k in sourcekeys if k==group+': stop')
        for name,c in channels.items():
            sc=controllers(source).get(name)
            if sc is None:raise ValueError('Missing deflection bone: '+name)
            for field in ('rotations','translations','scales'):
                target=getattr(c.data,field);original=target.keys
                if not len(original):continue
                values=getattr(sc.data,field).keys
                values=values[(values[:,0]>=start-1e-5)&(values[:,0]<=stop+1e-5)].copy()
                if not len(values):raise ValueError('Missing deflection track: '+name+':'+field)
                values[:,0]+=cursor-start
                target.keys=np.concatenate([original,values])
            c.stop_time=max(float(c.stop_time),cursor+stop-start)
        marks.extend([(cursor,'weapononehand: '+key+' start'),(cursor+stop-start,'weapononehand: '+key+' stop')])
        # Preserve source-requested item orientation changes while retaining the
        # already installed per-mount primitive clocks and calibrated native grips.
        for t,k in sourcekeys:
            if start<=t<=stop and ': dagger item 'in k:
                marks.append((cursor+t-start,'weapononehand: '+k.split(': ',1)[1]))
        report.append(dict(key=key,start=cursor,stop=cursor+stop-start,source=str(sourcepath),source_sha256=sha(sourcepath),rate=1.))
        cursor+=stop-start+.1
    ex.keys=np.asarray(sorted(marks,key=lambda row:row[0]),dtype='<f,O')
    s.save(path)
    reread=controllers(load(path));preserved=0
    for (name,field),original in before.items():
        after=getattr(reread[name].data,field).keys
        if after[:len(original)].tobytes()!=original.tobytes():raise ValueError('Existing key changed')
        preserved+=len(original)
    if [(float(t),str(k))for t,k in extra(load(path)).keys if float(t)<=max(t for t,_ in oldmarks)]!=oldmarks:
        raise ValueError('Existing gameplay marker changed')
    yaml=path.with_suffix('.yaml');rules=yaml.read_text()
    for key in ('bounce','bouncel','bouncer'):
        rules+=(f"  - from: '*'\n    to: 'weapononehand:{key} start'\n    easing: linear\n"
                "    duration: 0.06\n    clock: animation\n"
                f"  - from: ''\n    to: 'weapononehand:{key} start'\n    easing: linear\n    duration: 0\n")
    yaml.write_text(rules)
    outputs={p.relative_to(data).as_posix():sha(p)for p in (path,yaml)}
    manifest['outputs'].update(outputs)
    return dict(sections=report,preserved_existing_channel_keys=preserved,existing_markers_unchanged=True,
                original_global_stop_unchanged=True,outputs=outputs)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists():p.error('Fresh output directory required')
    shutil.copytree(a.input,a.output)
    manifest=json.loads((a.output/'weapon-families-import.json').read_text())
    report=dict(converter=Path(__file__).name,input=str(a.input),families={})
    for family in ('staff','dagger'):report['families'][family]=apply(a.output,family,manifest)
    manifest['family_wall_recoil']=report
    (a.output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
    (a.output/'family-wall-recoil-import.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))

if __name__=='__main__':main()
