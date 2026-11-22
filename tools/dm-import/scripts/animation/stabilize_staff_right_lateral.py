#!/usr/bin/env python3
"""Replace surplus right-arm lateral windup/recovery turns with direct returns.

Writes a fresh output directory. Both hit poses and the entire strike segment
stay exact, as do existing key times, text events, other attacks and the
left-arm return.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import numpy as np
from audit_staff_mesh_grip import slerp
from audit_viewmodel_continuity import tracks, angle
from audit_dagger_anatomy import sample
from retarget_kf import matrix_quat
from es3 import nif

ARM={f'Bip01 R {part}' for part in ('Clavicle','UpperArm','Forearm','Hand')}
FINGERS={f'Bip01 R Finger{i}{suffix}' for i in range(5) for suffix in ('','1','2')}
CHANGED=ARM|FINGERS|{'Weapon Bone'}

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def smooth(t):
    t=np.clip(t,0.,1.)
    return t*t*(3.-2.*t)

def patch(source,output):
    if output.exists(): raise ValueError('Fresh output required: '+str(output))
    rel=Path('Animations/brutal_fp_staff/weapononehand.kf')
    original=source/rel
    stream=nif.NiStream();stream.load(original)
    labels=[(float(t),str(k)) for t,k in stream.root.extra_data.keys]
    marks={k.removeprefix('weapononehand: '):t for t,k in labels}
    before=tracks(original)
    grid=before['Bip01 R Hand'][0][:,0]
    intervals=[(marks['slash start'],marks['slash max attack']),
               (float(grid[np.searchsorted(grid,marks['slash hit2']+.05)]),marks['slash large follow stop'])]
    shutil.copytree(source,output)
    names=[e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
    controllers=dict(zip(names,stream.root.controllers))
    changed=0
    for start,stop in intervals:
        first,last=sample(before,start),sample(before,stop)
        for name in CHANGED:
            controller=controllers[name]
            rotations=controller.data.rotations.keys.copy()
            positions=controller.data.translations.keys.copy()
            if not np.array_equal(rotations[:,0],positions[:,0]): raise ValueError('Grid mismatch '+name)
            a,b=first[name],last[name]
            for i in np.flatnonzero((rotations[:,0]>=start-1e-6)&(rotations[:,0]<=stop+1e-6)):
                weight=float(smooth((float(rotations[i,0])-start)/(stop-start)))
                rotations[i,1:]=slerp(matrix_quat(a[0]),matrix_quat(b[0]),weight)
                positions[i,1:]=(1.-weight)*a[1]+weight*b[1]
                changed+=1
            controller.data.rotations.keys=rotations
            controller.data.translations.keys=positions
    stream.save(output/rel)
    check=nif.NiStream();check.load(output/rel)
    if labels!=[(float(t),str(k)) for t,k in check.root.extra_data.keys]: raise ValueError('Gameplay events changed')
    after=tracks(output/rel)
    proof={}
    for name,(rotations,positions) in before.items():
        for old,new in zip((rotations,positions),after[name]):
            if not np.array_equal(old[:,0],new[:,0]): raise ValueError('Clock changed '+name)
            outside=np.ones(len(old),dtype=bool)
            if name in CHANGED:
                for a,b in intervals: outside&=(old[:,0]<a-1e-6)|(old[:,0]>b+1e-6)
            if not np.array_equal(old[outside],new[outside]): raise ValueError('Retained pose changed '+name)
        if name in ARM:
            proof[name]=[]
            for start,stop in intervals:
                row={}
                for label,data in [('before',rotations),('after',after[name][0])]:
                    q=data[(data[:,0]>=start-1e-6)&(data[:,0]<=stop+1e-6),1:]
                    row[label+'_angular_path_degrees']=float(angle(q[:-1],q[1:]).sum())
                proof[name].append(row)
    manifest=json.loads((source/'weapon-families-import.json').read_text())
    report=dict(source=str(source.resolve()),output=str(output.resolve()),file=rel.as_posix(),
                mode='direct-right-lateral-windup-and-return',intervals=intervals,
                changed_tracks=sorted(CHANGED),changed_keys=changed,angular_paths=proof,
                source_sha256=sha(original),output_sha256=sha(output/rel),recipe_sha256=sha(Path(__file__)),
                gameplay_keys_unchanged=True,clocks_unchanged=True,
                retained='Left arm, full strike from max attack through second hit +50ms, other attacks and hold loops',
                status='applied')
    manifest['outputs'][rel.as_posix()]=report['output_sha256']
    manifest['staff']['right_lateral_stabilization']=report
    (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
    (output/'staff-right-lateral-import.json').write_text(json.dumps(report,indent=2)+'\n')
    report['manifest_sha256']=sha(output/'weapon-families-import.json')
    return report

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();print(json.dumps(patch(a.input,a.output),indent=2))
if __name__=='__main__': main()
