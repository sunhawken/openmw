#!/usr/bin/env python3
"""Bake a native wrist pose preference across the complete dagger animation set.

The input is a complete dataset whose arm skin frames are already calibrated.
Only local Hand rotation keys change; fingers, grip carriers, item primitives,
translations, events and animation clocks remain unchanged. The preference is a
visual one, not a literal Source joint-angle correspondence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from audit_staff_mesh_grip import Rig
from calibrate_arm_frames import read, signature
from import_viewmodel import frame
from native_wrist_profile import lateral_cap
from retarget_kf import matrix_quat, quat_matrix


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--limit',default=30.,type=float)
    parser.add_argument('--halfwidth',default=5.,type=float)
    args=parser.parse_args()
    source,target=args.input.resolve(),args.output.resolve()
    if target.exists() or source==target or source in target.parents:
        parser.error('Use a fresh output outside the input dataset')
    if not 0<args.halfwidth<args.limit<90:
        parser.error('Require 0 < halfwidth < limit < 90 degrees')
    skeleton=source/'meshes/brutal_fp/xbase_anim.1st.nif'
    if not skeleton.is_file() or not (source/'arm-frame-calibration-import.json').is_file():
        parser.error('Input must be a complete dataset with calibrated native arm frames')
    if (source/'native-dagger-wrist-profile.json').exists():
        parser.error('Input already has this wrist profile; do not apply twice')
    files=sorted((source/'Animations/brutal_fp_dagger').glob('*.kf'))
    if not files:
        parser.error('No dagger KFs')
    rig=Rig(skeleton);palms={}
    for side in 'LR':
        roots=np.asarray([rig.nodes[f'Bip01 {side} Finger{i}'].translation for i in range(1,5)],float)
        palms[side]=frame(roots.mean(0),roots[3]-roots[0])
    shutil.copytree(source,target)
    report=dict(status='applied',input=str(source),output=str(target),
                recipe_sha256=sha(Path(__file__)),policy_sha256=sha(Path(__file__).with_name('native_wrist_profile.py')),
                limit_degrees=args.limit,transition_halfwidth_degrees=args.halfwidth,
                anatomical_zero='Geometric MCP landmark frame; artistic pose preference, not clinical calibration',
                preserved=['all event labels/times','all key timestamps','all other local channels',
                           'local fingers and grip carriers','independent item rotation/translation primitives',
                           'Hand origins','skeleton and skin binds'],files={})
    hands={f'Bip01 {side} Hand' for side in 'LR'}
    for path in files:
        stream,names,controllers=read(path);original_signature=signature(stream,names,controllers,hands)
        changes={}
        for side in 'LR':
            name=f'Bip01 {side} Hand';controller=controllers[names.index(name)]
            rows=controller.data.rotations.keys.copy();positions=controller.data.translations.keys
            count=0;maximum=0.
            for row in rows:
                rotation=quat_matrix(row[[2,3,4,1]])
                translation=np.asarray([np.interp(row[0],positions[:,0],positions[:,i]) for i in range(1,4)])
                correction,amount=lateral_cap(rotation,translation,palms[side],args.limit,args.halfwidth)
                if abs(amount)<=1e-7:
                    continue
                q=matrix_quat(rotation@correction)
                if q@row[1:]<0:q=-q
                row[1:]=q;count+=1;maximum=max(maximum,abs(amount))
            controller.data.rotations.keys=rows
            changes[side]=dict(changed_rotation_keys=count,maximum_correction_degrees=maximum)
        rel=path.relative_to(source);destination=target/rel
        if any(x['changed_rotation_keys'] for x in changes.values()):
            stream.save(destination)
        loaded=read(destination)
        if signature(*loaded,hands)!=original_signature:
            raise ValueError(f'Preserved fields changed: {rel}')
        report['files'][rel.as_posix()]=dict(changes=changes,input_sha256=sha(path),output_sha256=sha(destination))
    manifest_path=target/'weapon-families-import.json';manifest=json.loads(manifest_path.read_text())
    for rel,row in report['files'].items():
        manifest['outputs'][rel]=row['output_sha256']
    manifest['native_dagger_wrist_profile']=report
    if not all(sha(target/rel)==value for rel,value in manifest['outputs'].items()):
        raise ValueError('An existing output SHA does not match the complete dataset')
    manifest_path.write_text(json.dumps(manifest,indent=1)+'\n')
    (target/'native-dagger-wrist-profile.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(output=str(target),files=len(files),
        modified=sum(any(c['changed_rotation_keys'] for c in row['changes'].values()) for row in report['files'].values()),
        manifest_outputs=len(manifest['outputs'])),indent=2))


if __name__=='__main__':
    main()
