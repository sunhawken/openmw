#!/usr/bin/env python3
"""Restore Dark Messiah's fast staff choreography with a constant left-hand map.

Recovers the B/C/D attacks and the B/C returns from their source, then rebakes
only the left anatomical arm and fingers at the existing clocks. Other animation
data stays exact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import numpy as np
from import_staff_native_grasp import StaffNativeGrasp
from audit_viewmodel_continuity import tracks
from retarget_kf import matrix_quat
from stabilize_staff_left_windup import load

REL = Path('Animations/brutal_fp_staff/weapononehand.kf')
LEFT = {f'Bip01 L {part}' for part in ('Clavicle', 'UpperArm', 'Forearm', 'Hand')}
LEFT |= {f'Bip01 L Finger{i}{suffix}' for i in range(5) for suffix in ('', '1', '2')}

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def patch(runtime, original, output, vpk_root, work_dir, camera_contract, native):
    from es3 import nif
    if output.exists():
        raise ValueError('The output directory must not exist yet')
    streams = [load(root/REL) for root in (runtime, original)]
    labels = [[(float(t), str(k)) for t,k in s.root.extra_data.keys] for s in streams]
    assert labels[0] == labels[1], 'source/runtime text keys differ'
    marks = {k.removeprefix('weapononehand: '): t for t,k in labels[0]}
    source = tracks(original/REL)
    controllers = dict(zip([e.string_data for e in streams[0].root.extra_datas
                           if isinstance(e, nif.NiStringExtraData)], streams[0].root.controllers))
    vm = StaffNativeGrasp(vpk_root, work_dir,
        native/'xbase_anim.1st.nif', native/'xbase_anim.1st.kf',
        camera_contract=camera_contract,
        native_eye_height=True, grasp_profile=original/'staff-native-grasp-profile.json')
    vm.grip_path=native/'xbase_anim.1st.kf'; vm.configure('staff')
    intervals=[]
    for letter in 'BCD':
        intervals.append((f'fast{letter.lower()}', f'anm_fastattack_staff_{letter}'))
    for letter in 'BC':
        intervals.append((f'fastend{letter.lower()}', f'anm_fastattack_staff_{letter}_end'))
    reports=[]
    for section,clip in intervals:
        start,stop=marks[section+' start'],marks[section+' stop']
        grid=source['Bip01 L Hand'][0][:,0]
        indices=np.flatnonzero((grid >= start-1e-5)&(grid <= stop+1e-5))
        # Restore every authored fast channel, including right-hand/weapon
        # flourishes that the common preparation/held-hand bake removes.
        for name,c in controllers.items():
            for field,j in [('rotations',0),('translations',1)]:
                keys=getattr(c.data,field).keys
                assert np.array_equal(keys[:,0],source[name][j][:,0])
                keys[indices]=source[name][j][indices]
        state={}; previous={}
        for i in indices:
            time=float(np.clip(float(grid[i])-start,0,vm.duration('first_animations_staff',clip)))
            vm.source_clip=('first_animations_staff',clip,time)
            dm=vm.dm_world('first_animations_staff',clip,time)
            pose=vm.finish_pose(vm.solve(dm,state),dm)
            for name in LEFT:
                q=matrix_quat(pose[name][0])
                if name in previous and previous[name]@q<0:q=-q
                previous[name]=q
                controllers[name].data.rotations.keys[i,1:]=q
                controllers[name].data.translations.keys[i,1:]=pose[name][1]
        reports.append(dict(section=section,clip=clip,start=start,stop=stop,
                            source_rate=vm.source_rate('first_animations_staff',clip),keys=len(indices)))
    from staff_hand_clearance import adapt
    clearance=adapt(controllers, original/'meshes/brutal_fp/xbase_anim.1st.nif',native,reports)
    shutil.copytree(runtime,output)
    # These reports describe the replaced sections, not the restored animation.
    superseded=['staff-fast-c-import.json','staff-fast-c-targets.json']
    for name in superseded:
        stale=output/name
        if stale.exists():stale.unlink()
    streams[0].save(output/REL)
    report=dict(runtime=str(runtime.resolve()),original=str(original.resolve()),output=str(output.resolve()),
        file=REL.as_posix(),sections=reports,rebaked_left_channels=sorted(LEFT),
        retained='All other animation intervals/files, key clocks, text keys, native binds and lengths. B/C/D and B/C-return right arm/item/finger channels restored exactly from pre-simplification data.',
        source_sha256=sha(original/REL),runtime_sha256=sha(runtime/REL),output_sha256=sha(output/REL),
        recipe_sha256=sha(Path(__file__)),grasp_recipe_sha256=sha(Path(__file__).with_name('import_staff_native_grasp.py')),
        clearance_recipe_sha256=sha(Path(__file__).with_name('staff_hand_clearance.py')),native_hand_clearance=clearance,
        removed_superseded_metadata=superseded+['staff.fast_c_gesture','staff.fast_c_stabilization'],
        policy='DM source choreography and constant physical palm map; source-pose contact classification, direct anatomical target, and measured native hand-size clearance. No common ready pose, hand hold, temporal rewrite, or angular-path cap.')
    manifest=json.loads((output/'weapon-families-import.json').read_text())
    manifest['outputs'][REL.as_posix()]=report['output_sha256']
    manifest['staff']['dm_fast_restoration']=report
    manifest['staff'].pop('fast_c_stabilization',None)
    manifest['staff'].pop('fast_c_gesture',None)
    (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
    (output/'staff-dm-fast-import.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for key in ('runtime','original','output','vpk-root','work-dir','camera-contract','native'):
        p.add_argument('--'+key,type=Path,required=True)
    a=p.parse_args()
    print(json.dumps(patch(a.runtime,a.original,a.output,a.vpk_root,a.work_dir,a.camera_contract,a.native),indent=2))
