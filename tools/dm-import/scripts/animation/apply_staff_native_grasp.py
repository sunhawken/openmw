#!/usr/bin/env python3
"""Propagate one native Staff grasp through every existing FP Staff animation.

Gameplay text keys, source playback clocks, right arm, item and camera tracks
are retained. Only the left arm and its fifteen finger tracks are replaced.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import numpy as np
from import_staff_native_grasp import StaffNativeGrasp
from audit_dagger_anatomy import sample
from audit_viewmodel_continuity import tracks
from retarget_kf import matrix_quat

LEFT = {'Bip01 L '+n for n in ('Clavicle','UpperArm','Forearm','Hand')}
LEFT |= {f'Bip01 L Finger{i}'+s for i in range(5) for s in ('','1','2')}
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('profile','vpk-root','work-dir','skeleton','grip-animation','camera-contract'):
        p.add_argument('--'+name,required=True,type=Path)
    for name in ('input','output'):
        p.add_argument('--'+name,type=Path)
    p.add_argument('--generated',type=Path,help='Folder of the freshly generated staff set; built here first when it is empty')
    p.add_argument('--generate-only',action='store_true',help='Only build --generated, which needs no input set, then stop')
    p.add_argument('--staff-mount-offset',type=float,default=20.,help='Mount offset for --generate-only; otherwise read from the input set')
    p.add_argument('--right-rest-calibration', type=Path, help='Only for R frames without the anatomical solve; never apply to the anatomical L kernel')
    a=p.parse_args()
    if a.generate_only:
        if a.generated is None: p.error('--generate-only needs --generated')
        m={'staff':{'staff_mount_offset':a.staff_mount_offset}}
    else:
        if a.input is None or a.output is None: p.error('--input and --output are required')
        if a.output.exists(): raise ValueError('Fresh output required')
        m=json.loads((a.input/'weapon-families-import.json').read_text())
    vm=StaffNativeGrasp(a.vpk_root,a.work_dir,a.skeleton,a.grip_animation,
        camera_contract=a.camera_contract,native_eye_height=True,grasp_profile=a.profile)
    vm.grip_path=a.grip_animation;vm.staff_mount_offset=float(m['staff'].get('staff_mount_offset',20.));vm.configure('staff')
    right_factors = {}
    if a.right_rest_calibration:
        if m.get('constant_arm_frame_calibration'): raise ValueError('Input already has constant arm-frame correction; omit R correction')
        from calibrate_arm_frames import load_calibration, corrections
        _, calibration = load_calibration(a.right_rest_calibration, a.skeleton)
        right_factors = corrections({'R': calibration['R']})
    generated=a.generated or a.output.parent/(a.output.name+'-generated-staff')
    groups=generated/'staff-groups.json'
    if groups.is_file():
        stored=json.loads(groups.read_text())
        if stored['staff_mount_offset']!=vm.staff_mount_offset: raise ValueError('Generated staff set uses another mount offset')
        freshmeta=stored['groups']
    else:
        generated.mkdir(parents=True,exist_ok=False)
        vm.build(generated)
        vm.clinch_set(generated/'Animations/brutal_fp_staff')
        freshmeta=copy.deepcopy(vm.groups['brutal_fp_staff'])
        # Written last: its presence marks a complete set that a later run may reuse.
        groups.write_text(json.dumps(dict(staff_mount_offset=vm.staff_mount_offset,groups=freshmeta),default=float)+'\n')
    if a.generate_only: return
    fresh={q.stem:tracks(q)for q in(generated/'Animations/brutal_fp_staff').glob('*.kf')}
    freshmarks={g:dict((label,float(time)) for time,label in info['marks'])for g,info in freshmeta.items()}
    def stream(path):
        s=vm.nif.NiStream();s.load(path);return s
    def marks(s):
        return [(float(t),str(k))for t,k in s.root.extra_data.keys]
    def pairs(s):
        return dict(zip([e.string_data for e in s.root.extra_datas if isinstance(e,vm.nif.NiStringExtraData)],s.root.controllers))
    weapon=stream(a.input/'Animations/brutal_fp_staff/weapononehand.kf')
    oldmarks=dict((label.removeprefix('weapononehand: '),t)for t,label in marks(weapon))
    newmarks=freshmarks['weapononehand']
    ranges=[]
    for phase in ('equip','unequip'):
        ranges.append(dict(label=phase,old=(oldmarks[phase+' start'],oldmarks[phase+' stop']),new=(newmarks[phase+' start'],newmarks[phase+' stop'])))
    for attack in ('chop','slash','thrust','neutral'):
        ranges.extend([dict(label=attack+' windup',old=(oldmarks[attack+' start'],oldmarks[attack+' max attack']),new=(newmarks[attack+' start'],newmarks[attack+' max attack'])),
            dict(label=attack+' out',old=(oldmarks[attack+' min hit'],oldmarks[attack+' large follow stop']),new=(newmarks[attack+' min hit'],newmarks[attack+' large follow stop']))])
    for key in [f'fast{x}'for x in 'abcd']+[f'fastend{x}'for x in 'abc']:
        ranges.append(dict(label=key,old=(oldmarks[key+' start'],oldmarks[key+' stop']),new=(newmarks[key+' start'],newmarks[key+' stop'])))
    # Charged holds lie after the global stop; recreate their
    # poses at their existing clocks through this same arm/finger factory.
    held={}
    for row in m['staff']['groups']['brutal_fp_staff']['weapononehand']['segments']:
        if not row['clip'].startswith('anm_bigslash_staff')or not row['clip'].endswith('_idle'):continue
        vm.pending=[]
        loop=bool(row.get('loop'));lead=float(row.get('loop_lead_in',0.))
        duration=float(row['loop_period'])if loop else float(row['duration'])
        times,poses,_=vm.segment(row['model'],row['clip'],duration=duration,loop=loop,loop_lead_in=lead,runtime_entry=True,roll_state={})
        held[row['offset']]=dict(row=row,times=times,poses=poses)
        ranges.append(dict(label=row['clip'],old=(row['offset'],row['offset']+row['duration']),hold=row['offset']))
    shutil.copytree(a.input,a.output)
    report=dict(source=str(a.input),output=str(a.output),profile=json.loads(a.profile.read_text()),rows=[],weapon_phase_maps=ranges,
                policy='One native left anatomical grasp across every Staff KF. Existing events, all channel clocks, right arm, item, camera, and other families remain unchanged.')
    for path in sorted((a.output/'Animations/brutal_fp_staff').glob('*.kf')):
        group=path.stem;s=stream(path);old=stream(a.input/path.relative_to(a.output));controllers=pairs(s);before=pairs(old)
        target_times=controllers['Bip01 L Hand'].data.rotations.keys[:,0]
        poses=[]
        if group!='weapononehand':
            for t in target_times:poses.append(sample(fresh[group],float(t)))
        else:
            for time in target_times:
                t=float(time);matches=[r for r in ranges if r['old'][0]-2e-5<=t<=r['old'][1]+2e-5]
                if not matches:raise ValueError('Unmapped existing Staff weapon clock: '+str(t))
                r=matches[0];local=max(0.,t-r['old'][0])
                if 'hold'in r:
                    h=held[r['hold']];ts=h['times'];j=int(np.searchsorted(ts,local,side='right')-1);j=max(0,min(j,len(ts)-1))
                    if j+1<len(ts)and ts[j+1]>ts[j]:pose=vm.blend_pose(h['poses'][j],h['poses'][j+1],float(np.clip((local-ts[j])/(ts[j+1]-ts[j]),0,1)))
                    else:pose=h['poses'][j]
                else:pose=sample(fresh[group],r['new'][0]+local)
                poses.append(pose)
        for name in LEFT:
            c=controllers[name]
            # Existing arrays retain their exact sample clocks. Rotation and
            # translation clocks in these native Staff sources are identical.
            if not np.array_equal(c.data.rotations.keys[:,0],c.data.translations.keys[:,0]):raise ValueError('Mismatched native key clocks: '+name)
            q=np.asarray([matrix_quat(pose[name][0])for pose in poses])
            for i in range(1,len(q)):
                if q[i-1]@q[i]<0:q[i]*=-1
            c.data.rotations.keys[:,1:]=q.astype(np.float32)
            c.data.translations.keys[:,1:]=np.asarray([pose[name][1]for pose in poses],np.float32)
        if right_factors:
            from calibrate_arm_frames import rotate_keys
            for name, factors in right_factors.items():
                keys = controllers[name].data.rotations.keys
                controllers[name].data.rotations.keys = rotate_keys(keys.astype(float), *factors).astype(keys.dtype)
        s.save(path)
        after=pairs(stream(path));preserved=0
        for name,c in before.items():
            for channel in ('rotations','translations','scales'):
                x,y=getattr(c.data,channel).keys,getattr(after[name].data,channel).keys
                if not np.array_equal(x[:,0],y[:,0]):raise ValueError('Changed channel clock')
                if (name not in LEFT and not (name in right_factors and channel=='rotations')) or channel=='scales':
                    if x.tobytes()!=y.tobytes():raise ValueError('Changed retained channel: '+name)
                    preserved+=len(x)
        if marks(old)!=marks(stream(path)):raise ValueError('Changed gameplay text keys')
        relative=path.relative_to(a.output).as_posix();m['outputs'][relative]=sha(path)
        report['rows'].append(dict(file=relative,frames=len(target_times),other_channel_keys_byte_identical=preserved,gameplay_keys_unchanged=True,channel_clocks_unchanged=True,output_sha256=sha(path)))
    report['arm_frame_profile'] = {'L':'source-anatomical-arm-kernel plus native anatomical grasp; no constant CF', 'R':'constant-rest Cu/Cf; preserves Hand/finger/item world motion' if right_factors else 'input R frame convention preserved'}
    report['right_rest_calibration'] = {'path':str(a.right_rest_calibration), 'sha256':sha(a.right_rest_calibration)} if right_factors else None
    profile=json.loads(a.profile.read_text());shutil.copy2(a.profile,a.output/'staff-native-grasp-profile.json')
    m['staff']['native_grasp']=dict(profile=profile,arm_frame_profile=report['arm_frame_profile'],right_rest_calibration=report['right_rest_calibration'],scope='all27 Staff KFs including draw/ends/guards/clinch and charged holds',left_arm_kernel='retarget_anatomical_arm fixed Source Hand reference',source_role_policy='Contact correction fades with source grasp/open role; timing preserved',converter=Path(__file__).name,recipe_sha256=sha(Path(__file__)))
    m['staff']['superseded_male_fit']={k:m['staff'][k]for k in ('staff_grip_fit','staff_grip_fit_axes','staff_anatomical_map','staff_anatomical_anchor')if k in m['staff']}
    for key in ('staff_grip_fit','staff_grip_fit_axes','staff_anatomical_map','staff_anatomical_anchor'):m['staff'].pop(key,None)
    (a.output/'weapon-families-import.json').write_text(json.dumps(m,indent=1)+'\n')
    report['manifest_sha256']=sha(a.output/'weapon-families-import.json')
    report['groups']=len(report['rows']);report['other_families_files_byte_identical']=all(sha(q)==sha(a.output/q.relative_to(a.input))for q in a.input.rglob('*')if q.is_file()and q.relative_to(a.input).parts[0]=='Animations'and q.relative_to(a.input).parts[1]!='brutal_fp_staff')
    (a.output/'staff-native-grasp-import.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items()if k not in ('rows','profile','weapon_phase_maps')},indent=2))

if __name__=='__main__':main()
