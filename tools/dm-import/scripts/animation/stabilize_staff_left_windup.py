#!/usr/bin/env python3
"""Remove the left arm's axial windup flourish without moving its shoulder.

Bakes one direct wrist release into a fresh polearm output directory. The right
arm/shaft, charged hold, both hit poses/events and the return stay exact. No
runtime IK or bone-length changes are introduced.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import numpy as np
from retarget_kf import cross, norm
from audit_staff_mesh_grip import Rig, slerp
from audit_viewmodel_continuity import tracks, angle
from import_staff_native_grasp import swing
from retarget_kf import matrix_quat, quat_matrix

ARM={f'Bip01 L {part}' for part in ('UpperArm','Forearm','Hand')}
FINGERS={f'Bip01 L Finger{i}{suffix}' for i in range(5) for suffix in ('','1','2')}
CHANGED=ARM|FINGERS
REL=Path('Animations/brutal_fp_staff/weapononehand.kf')
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def smooth(x):
    x=float(np.clip(x,0,1));return x*x*(3-2*x)
def mix(a,b,w):return quat_matrix(slerp(matrix_quat(a),matrix_quat(b),w)[[1,2,3,0]])
def unit(v):return v/norm(v)
def axial(axis,theta):
    x,y,z=axis;K=np.array([[0.,-z,y],[z,0.,-x],[-y,x,0.]])
    return np.eye(3)+np.sin(theta)*K+(1-np.cos(theta))*(K@K)
def twist_angle(R,axis):
    skew=np.array([R[2,1]-R[1,2],R[0,2]-R[2,0],R[1,0]-R[0,1]])*.5
    return np.arctan2(np.dot(axis,skew),(np.trace(R)-1)*.5)
def transport_rotation(A,B,start_axis,end_axis,current_axis,w):
    # Parallel transport, then the one bounded endpoint roll. Interpolating an
    # elbow reference through the limb axis would flip its bend plane mid-clip.
    end=swing(start_axis,end_axis)@A
    theta=twist_angle(B@end.T,end_axis)
    return axial(current_axis,w*theta)@swing(start_axis,current_axis)@A
def load(p):
    from es3 import nif
    s=nif.NiStream();s.load(p);return s

def patch(source,output):
    if output.exists():raise ValueError('Fresh output required')
    from es3 import nif
    stream=load(source/REL);before=tracks(source/REL)
    labels=[(float(t),str(k))for t,k in stream.root.extra_data.keys]
    marks={k.removeprefix('weapononehand: '):t for t,k in labels}
    U,F,H,C=[f'Bip01 L {b}' for b in ('UpperArm','Forearm','Hand','Clavicle')]
    grid=before[H][0][:,0]
    first=int(np.argmin(abs(grid-marks['slash start'])))
    last=int(np.argmin(abs(grid-marks['slash max attack'])))
    start,stop=float(grid[first]),float(grid[last])
    rig=Rig(source/'meshes/brutal_fp/xbase_anim.1st.nif')
    A,B=rig.world(before,first),rig.world(before,last)
    a_axis,b_axis=[unit(v[H][:3,3]-v[U][:3,3])for v in (A,B)]
    def bend_of(v,axis):
        bend=v[F][:3,3]-v[U][:3,3];return unit(bend-axis*np.dot(bend,axis))
    a_bend,b_bend=bend_of(A,a_axis),bend_of(B,b_axis)
    end_bend=swing(a_axis,b_axis)@a_bend
    bend_twist=np.arctan2(np.dot(b_axis,cross(end_bend,b_bend)),np.dot(end_bend,b_bend))
    names=[e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
    controllers=dict(zip(names,stream.root.controllers));previous={};clamps=[]
    for i in range(first,last+1):
        w=smooth((float(grid[i])-start)/(stop-start));world=rig.world(before,i)
        target=(1-w)*A[H][:3,3]+w*B[H][:3,3]
        hand=mix(A[H][:3,:3],B[H][:3,:3],w)
        S=world[U][:3,3]
        l1,l2=[float(norm(before[b][1][i,1:]))for b in (F,H)]
        delta=target-S;distance=float(norm(delta));axis=delta/max(distance,1e-12)
        bounded=float(np.clip(distance,abs(l1-l2)+1e-5,l1+l2-1e-5));clamps.append(abs(distance-bounded))
        target=S+axis*bounded
        bend=axial(axis,w*bend_twist)@swing(a_axis,axis)@a_bend
        along=(l1*l1-l2*l2+bounded*bounded)/(2*bounded)
        elbow=S+axis*along+bend*np.sqrt(max(0,l1*l1-along*along))
        upper=transport_rotation(A[U][:3,:3],B[U][:3,:3],
            unit(A[F][:3,3]-A[U][:3,3]),unit(B[F][:3,3]-B[U][:3,3]),unit(elbow-S),w)
        fore=transport_rotation(A[F][:3,:3],B[F][:3,:3],
            unit(A[H][:3,3]-A[F][:3,3]),unit(B[H][:3,3]-B[F][:3,3]),unit(target-elbow),w)
        for bone,R in {U:world[C][:3,:3].T@upper,F:upper.T@fore,H:fore.T@hand}.items():
            q=matrix_quat(R)
            if bone in previous and np.dot(previous[bone],q)<0:q=-q
            previous[bone]=q;controllers[bone].data.rotations.keys[i,1:]=q
        # Release once, near the start, rather than carrying the flourish's
        # independently oscillating open/closed finger performance into the bake.
        release=smooth((float(grid[i])-start)/.22)
        for bone in FINGERS:
            for channel,j in (('rotations',0),('translations',1)):
                a,b=before[bone][j][first,1:],before[bone][j][last,1:]
                getattr(controllers[bone].data,channel).keys[i,1:]=slerp(a,b,release)if j==0 else (1-release)*a+release*b
    for bone in CHANGED:
        for channel,j in (('rotations',0),('translations',1)):
            keys=getattr(controllers[bone].data,channel).keys
            keys[first]=before[bone][j][first];keys[last]=before[bone][j][last]
    shutil.copytree(source,output);stream.save(output/REL);after=tracks(output/REL)
    assert labels==[(float(t),str(k))for t,k in load(output/REL).root.extra_data.keys]
    for bone,pair in before.items():
        for old,new in zip(pair,after[bone]):
            assert np.array_equal(old[:,0],new[:,0]),bone+' clocks'
            retained=np.ones(len(old),bool)
            if bone in CHANGED:retained[first:last+1]=False
            assert np.array_equal(old[retained],new[retained]),bone+' retained poses'
    for bone in ARM:assert np.array_equal(before[bone][1],after[bone][1]),bone+' origin'
    paths={}
    for label,channels in (('before',before),('after',after)):
        worlds=[rig.world(channels,i) for i in range(first,last+1)]
        paths[label]={}
        for bone in ARM:
            q=np.array([matrix_quat(v[bone][:3,:3])for v in worlds])
            paths[label][bone]=dict(path_degrees=float(angle(q[:-1],q[1:]).sum()),direct_degrees=float(angle(q[0:1],q[-1:])[0]))
    report=dict(source=str(source.resolve()),output=str(output.resolve()),file=REL.as_posix(),start=start,stop=stop,
        changed_tracks=sorted(CHANGED),clavicles_and_origins_unchanged=True,right_arm_and_weapon_unchanged=True,
        hits_hold_return_keys_clocks_unchanged=True,max_reach_clamp=max(clamps),world_angular_paths=paths,
        source_sha256=sha(source/REL),output_sha256=sha(output/REL),recipe_sha256=sha(Path(__file__)),
        status='applied')
    manifest=json.loads((source/'weapon-families-import.json').read_text())
    manifest['outputs'][REL.as_posix()]=report['output_sha256'];manifest['staff']['left_windup_stabilization']=report
    (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
    (output/'staff-left-windup-import.json').write_text(json.dumps(report,indent=2)+'\n')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',required=True,type=Path);p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();print(json.dumps(patch(a.input,a.output),indent=2))
