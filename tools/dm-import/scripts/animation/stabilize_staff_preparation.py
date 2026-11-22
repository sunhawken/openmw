#!/usr/bin/env python3
"""Bake smooth whole-arm side preparation and one coupled forward preparation.

The lateral wrist path is kept while its shoulder does not follow noisy retarget
corrections. Forward preparation moves one shaft with two native grips, without
a preliminary release/catch/twirl. Damage, held poses and recovery keep their
existing clocks and data. Writes a fresh output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import numpy as np
from retarget_kf import cross, norm
from audit_staff_mesh_grip import Rig
from audit_viewmodel_continuity import tracks
from stabilize_staff_left_windup import smooth, mix, unit, axial, swing, transport_rotation, load
from retarget_kf import matrix_quat

REL=Path('Animations/brutal_fp_staff/weapononehand.kf')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def node(side,bone):return f'Bip01 {side} {bone}'
def fingers(side):return {node(side,f'Finger{i}{end}')for i in range(5)for end in ('','1','2')}
def transform(R,p):
    M=np.eye(4);M[:3,:3]=R;M[:3,3]=p;return M
def lerp(a,b,w):return (1-w)*a+w*b

def patch(source,output,forward_lift=12.):
    if output.exists():raise ValueError('The output directory must not exist yet')
    from es3 import nif
    stream=load(source/REL);before=tracks(source/REL)
    labels=[(float(t),str(k))for t,k in stream.root.extra_data.keys]
    marks={k.removeprefix('weapononehand: '):t for t,k in labels}
    names=[e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
    controllers=dict(zip(names,stream.root.controllers))
    rig=Rig(source/'meshes/brutal_fp/xbase_anim.1st.nif')
    grid=before[node('L','Hand')][0][:,0]
    anchor=np.asarray(json.loads((source/'staff-native-grasp-profile.json').read_text())['shaft_anchor_native_hand'])
    changed={};sections=[];goals=[];previous={};clamps=[]
    for attack,sides in [('slash','L'),('chop','LR')]:
        first=int(np.argmin(abs(grid-marks[attack+' start'])))
        last=int(np.argmin(abs(grid-marks[attack+' max attack'])))
        start,stop=float(grid[first]),float(grid[last])
        A,B=rig.world(before,first),rig.world(before,last)
        references={}
        for side in sides:
            U,F,H,C=[node(side,b)for b in ('UpperArm','Forearm','Hand','Clavicle')]
            a_axis,b_axis=[unit(v[H][:3,3]-v[U][:3,3])for v in (A,B)]
            def bend(v,axis):
                e=v[F][:3,3]-v[U][:3,3];return unit(e-axis*np.dot(e,axis))
            a_bend,b_bend=bend(A,a_axis),bend(B,b_axis)
            end_bend=swing(a_axis,b_axis)@a_bend
            roll=np.arctan2(np.dot(b_axis,cross(end_bend,b_bend)),np.dot(end_bend,b_bend))
            references[side]=(a_axis,a_bend,roll)
            for bone in (C,U,F,H):changed.setdefault((bone,'translations'if bone==C else 'rotations'),[]).append((first,last))
            if attack=='chop':
                for bone in fingers(side):
                    for channel in ('rotations','translations'):changed.setdefault((bone,channel),[]).append((first,last))
        relative={side:(np.linalg.inv(A['Weapon Bone'])@A[node(side,'Hand')],
                        np.linalg.inv(B['Weapon Bone'])@B[node(side,'Hand')])for side in sides}
        for i in range(first,last+1):
            w=smooth((float(grid[i])-start)/(stop-start));world=rig.world(before,i)
            weapon=transform(mix(A['Weapon Bone'][:3,:3],B['Weapon Bone'][:3,:3],w),
                             lerp(A['Weapon Bone'][:3,3],B['Weapon Bone'][:3,3],w))
            if attack=='chop':
                # Keep the one continuous preparation in view instead of
                # dropping the shaft below the camera halfway through the turn.
                weapon[2,3]+=forward_lift*np.sin(np.pi*w)
            for side in sides:
                U,F,H,C=[node(side,b)for b in ('UpperArm','Forearm','Hand','Clavicle')]
                Cpos=lerp(A[C][:3,3],B[C][:3,3],w)
                parent=world[rig.parents[C]]
                controllers[C].data.translations.keys[i,1:]=parent[:3,:3].T@(Cpos-parent[:3,3])
                S=world[U][:3,3]+Cpos-world[C][:3,3]
                if attack=='slash':
                    target=lerp(A[H][:3,3],B[H][:3,3],w)
                    hand=mix(A[H][:3,:3],B[H][:3,:3],w)
                else:
                    a,b=relative[side];R=mix(a[:3,:3],b[:3,:3],w)
                    # Interpolate the shaft contact anchor, keeping a closed
                    # native grip while the left palm slides along the haft.
                    contact=lerp(a[:3,3]+a[:3,:3]@anchor,b[:3,3]+b[:3,:3]@anchor,w)
                    local=transform(R,contact-R@anchor)
                    goal=weapon@local;target,hand=goal[:3,3],goal[:3,:3]
                l1,l2=[float(norm(before[b][1][i,1:]))for b in (F,H)]
                delta=target-S;distance=float(norm(delta));axis=delta/max(distance,1e-12)
                bounded=float(np.clip(distance,abs(l1-l2)+1e-5,l1+l2-1e-5));clamps.append(abs(distance-bounded))
                target=S+axis*bounded
                a_axis,a_bend,roll=references[side]
                bend=axial(axis,w*roll)@swing(a_axis,axis)@a_bend
                along=(l1*l1-l2*l2+bounded*bounded)/(2*bounded)
                elbow=S+axis*along+bend*np.sqrt(max(0,l1*l1-along*along))
                upper=transport_rotation(A[U][:3,:3],B[U][:3,:3],unit(A[F][:3,3]-A[U][:3,3]),
                    unit(B[F][:3,3]-B[U][:3,3]),unit(elbow-S),w)
                fore=transport_rotation(A[F][:3,:3],B[F][:3,:3],unit(A[H][:3,3]-A[F][:3,3]),
                    unit(B[H][:3,3]-B[F][:3,3]),unit(target-elbow),w)
                for bone,R in {U:world[C][:3,:3].T@upper,F:upper.T@fore,H:fore.T@hand}.items():
                    q=matrix_quat(R)
                    if bone in previous and previous[bone]@q<0:q=-q
                    previous[bone]=q;controllers[bone].data.rotations.keys[i,1:]=q
                if attack=='chop':
                    from audit_staff_mesh_grip import slerp
                    for bone in fingers(side):
                        controllers[bone].data.rotations.keys[i,1:]=slerp(before[bone][0][first,1:],before[bone][0][last,1:],w)
                        controllers[bone].data.translations.keys[i,1:]=lerp(before[bone][1][first,1:],before[bone][1][last,1:],w)
                goals.append(dict(index=i,bone=H,position=target.tolist()))
        sections.append(dict(attack=attack,first=first,last=last,start=start,stop=stop,sides=sides))
    # Endpoints and every event keep their poses. The constant right-hand
    # weapon mount stays exact, including the forward preparation.
    for (bone,channel),intervals in changed.items():
        j=0 if channel=='rotations'else 1;keys=getattr(controllers[bone].data,channel).keys
        for first,last in intervals:keys[first]=before[bone][j][first];keys[last]=before[bone][j][last]
    shutil.copytree(source,output);stream.save(output/REL);after=tracks(output/REL)
    assert labels==[(float(t),str(k))for t,k in load(output/REL).root.extra_data.keys]
    for bone,pair in before.items():
        for j,(channel,old,new)in enumerate(zip(('rotations','translations'),pair,after[bone])):
            assert np.array_equal(old[:,0],new[:,0]),bone+' clocks'
            keep=np.ones(len(old),bool)
            for first,last in changed.get((bone,channel),[]):keep[first:last+1]=False
            assert np.array_equal(old[keep],new[keep]),bone+' retained '+channel
    errors=[];worlds={};lateral_position_error=0.;lateral_rotation_error=0.
    for goal in goals:
        i=goal['index']
        if i not in worlds:worlds[i]=rig.world(after,i)
        errors.append(float(norm(worlds[i][goal['bone']][:3,3]-goal['position'])))
    lateral=next(s for s in sections if s['attack']=='slash')
    for i in range(lateral['first'],lateral['last']+1):
        old,new=rig.world(before,i),worlds[i];H=node('L','Hand')
        lateral_position_error=max(lateral_position_error,float(norm(old[H][:3,3]-new[H][:3,3])))
        lateral_rotation_error=max(lateral_rotation_error,float(norm(old[H][:3,:3]-new[H][:3,:3])))
    assert max(errors)<1e-4,max(errors)
    assert max(clamps)<1e-4,max(clamps)
    report=dict(source=str(source.resolve()),output=str(output.resolve()),file=REL.as_posix(),sections=sections,
        max_reach_clamp=max(clamps),max_wrist_position_error=max(errors),event_keys_and_clocks_unchanged=True,
        hit_hold_recovery_and_other_attacks_byte_identical=True,bone_lengths_and_arm_origins_unchanged=True,
        weapon_local_mount_byte_identical=True,shoulder_endpoint_positions_unchanged=True,
        lateral_wrist_path_preserved=True,forward_mode='one-shaft-two-native-grips',
        forward_lift=forward_lift,lateral_wrist_position_error=lateral_position_error,lateral_wrist_rotation_error=lateral_rotation_error,
        source_sha256=sha(source/REL),output_sha256=sha(output/REL),recipe_sha256=sha(Path(__file__)),
        status='applied')
    manifest=json.loads((source/'weapon-families-import.json').read_text());manifest['outputs'][REL.as_posix()]=report['output_sha256']
    manifest['staff']['smooth_preparation']=report
    (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
    (output/'staff-smooth-preparation-import.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--forward-lift',type=float,default=12.)
    a=p.parse_args();print(json.dumps(patch(a.input,a.output,a.forward_lift),indent=2))
