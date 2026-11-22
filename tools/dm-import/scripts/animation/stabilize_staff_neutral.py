#!/usr/bin/env python3
"""Replace neutral charged polearm flourishes with one preparation and return.

Keeps exact hit/held poses, clocks, other attacks, weapon mount and arm origins,
and writes a fresh output directory. The left hand transfers once between
endpoint grips, with one opening and one near-contact closure.
"""
import argparse,hashlib,json,shutil
from pathlib import Path
import numpy as np
from retarget_kf import cross, norm
from audit_staff_mesh_grip import Rig,slerp
from audit_viewmodel_continuity import tracks,angle
from stabilize_staff_left_windup import load,smooth,mix,unit,axial,swing,transport_rotation
from retarget_kf import matrix_quat
REL=Path('Animations/brutal_fp_staff/weapononehand.kf')
def node(s,b):return f'Bip01 {s} {b}'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def patch(source,output,settle=.2,prep_lift=12.,return_lateral=.2):
    if output.exists():raise ValueError('The output directory must not exist yet')
    from es3 import nif
    before=tracks(source/REL);stream=load(source/REL);labels=[(float(t),str(k))for t,k in stream.root.extra_data.keys]
    marks={k.removeprefix('weapononehand: '):t for t,k in labels};grid=before[node('L','Hand')][0][:,0]
    key=lambda t:int(np.argmin(abs(grid-t)))
    phases=[('preparation',key(marks['neutral start']),key(marks['neutral max attack'])),
            ('return',key(marks['neutral hit2']+settle),key(marks['neutral large follow stop']))]
    controllers=dict(zip([e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)],stream.root.controllers))
    rig=Rig(source/'meshes/brutal_fp/xbase_anim.1st.nif');changed={};goals=[];clamps=[];receipts=[];sections=[]
    # Dark Messiah's free-hand neutral flourish supplies one relaxed finger pose.
    open_index=key(marks['neutral hit2']+.70)
    LH,RH=node('L','Hand'),node('R','Hand')
    free=rig.world(before,open_index);free_relative=np.linalg.inv(free['Weapon Bone'])@free[LH]
    assert norm(free_relative[[0,2],3])>20., 'Expected an actual free-hand reference'
    finger_reference_distances=[]
    for _,_,last in phases:
        distance=sum(float(angle(values[0][open_index:open_index+1,1:],values[0][last:last+1,1:])[0])
                     for name,values in before.items() if name.startswith('Bip01 L Finger'))
        assert distance>300., 'Expected relaxed fingers separated from either closed endpoint'
        finger_reference_distances.append(distance)
    for phase,first,last in phases:
        A,B=rig.world(before,first),rig.world(before,last);start,stop=float(grid[first]),float(grid[last]);refs={};previous={}
        mount=np.linalg.inv(A['Weapon Bone'])@A[RH]
        assert np.max(abs(mount-np.linalg.inv(B['Weapon Bone'])@B[RH]))<1e-5
        grip=np.linalg.inv(B[RH])@B[LH]
        for side in ('L','R'):
            U,F,H,C=[node(side,b)for b in ('UpperArm','Forearm','Hand','Clavicle')]
            axes=[unit(V[H][:3,3]-V[U][:3,3])for V in (A,B)]
            bends=[]
            for V,axis in zip((A,B),axes):
                v=V[F][:3,3]-V[U][:3,3];bends.append(unit(v-axis*np.dot(v,axis)))
            transported=swing(*axes)@bends[0];roll=np.arctan2(np.dot(axes[1],cross(transported,bends[1])),np.dot(transported,bends[1]));refs[side]=(axes[0],bends[0],roll)
            changed.setdefault((C,'translations'),[]).append((first,last))
            for bone in (U,F,H):changed.setdefault((bone,'rotations'),[]).append((first,last))
            for digit in range(5):
                for suffix in ('','1','2'):
                    for channel in ('rotations','translations'):changed.setdefault((node(side,f'Finger{digit}{suffix}'),channel),[]).append((first,last))
        for i in range(first,last+1):
            t=float(grid[i]);w=smooth((t-start)/(stop-start));W=np.eye(4);W[:3,:3]=mix(A['Weapon Bone'][:3,:3],B['Weapon Bone'][:3,:3],w);W[:3,3]=(1-w)*A['Weapon Bone'][:3,3]+w*B['Weapon Bone'][:3,3]
            if phase=='preparation':W[2,3]+=prep_lift*np.sin(np.pi*w)
            right=W@mount;left=(1-w)*A[LH][:3,3]+w*B[LH][:3,3]
            if phase=='return':
                early=smooth((t-start)/return_lateral);left[0]=(1-early)*A[LH][0,3]+early*B[LH][0,3]
            rotations={'R':right[:3,:3],'L':mix(A[LH][:3,:3],B[LH][:3,:3],w)}
            world=rig.world(before,i)
            for side,target in [('L',left),('R',right[:3,3])]:
                U,F,H,C=[node(side,b)for b in ('UpperArm','Forearm','Hand','Clavicle')];parent=world[rig.parents[C]]
                assert np.max(abs(parent-A[rig.parents[C]]))<1e-5
                Cpos=(1-w)*A[C][:3,3]+w*B[C][:3,3];controllers[C].data.translations.keys[i,1:]=parent[:3,:3].T@(Cpos-parent[:3,3]);S=world[U][:3,3]+Cpos-world[C][:3,3]
                l1,l2=[float(norm(before[b][1][i,1:]))for b in (F,H)];delta=target-S;distance=float(norm(delta));axis=unit(delta);bounded=float(np.clip(distance,abs(l1-l2)+1e-5,l1+l2-1e-5));clamps.append(abs(distance-bounded));target=S+axis*bounded
                a_axis,a_bend,roll=refs[side];bend=axial(axis,w*roll)@swing(a_axis,axis)@a_bend;along=(l1*l1-l2*l2+bounded*bounded)/(2*bounded);elbow=S+axis*along+bend*np.sqrt(max(0,l1*l1-along*along))
                upper=transport_rotation(A[U][:3,:3],B[U][:3,:3],unit(A[F][:3,3]-A[U][:3,3]),unit(B[F][:3,3]-B[U][:3,3]),unit(elbow-S),w)
                fore=transport_rotation(A[F][:3,:3],B[F][:3,:3],unit(A[H][:3,3]-A[F][:3,3]),unit(B[H][:3,3]-B[F][:3,3]),unit(target-elbow),w)
                for bone,R in {U:world[C][:3,:3].T@upper,F:upper.T@fore,H:fore.T@rotations[side]}.items():
                    q=matrix_quat(R)
                    if bone in previous and previous[bone]@q<0:q=-q
                    previous[bone]=q;controllers[bone].data.rotations.keys[i,1:]=q
                goals.append((i,H,target.copy(),S.copy()))
            gap=float(norm((right@grip)[:3,3]-left));closure=smooth(1-gap/6.)
            for side in ('L','R'):
                if side=='L' and t-start<.15:a,b,f=first,open_index,smooth((t-start)/.15)
                elif side=='L':a,b,f=open_index,last,closure
                else:a,b,f=first,last,w
                for digit in range(5):
                    for suffix in ('','1','2'):
                        H=node(side,f'Finger{digit}{suffix}');controllers[H].data.rotations.keys[i,1:]=slerp(before[H][0][a,1:],before[H][0][b,1:],f);controllers[H].data.translations.keys[i,1:]=(1-f)*before[H][1][a,1:]+f*before[H][1][b,1:]
            receipts.append(dict(phase=phase,t=t,progress=w,left_grip_gap=gap,closure=closure))
        sections.append(dict(phase=phase,first=first,last=last,start=start,stop=stop))
    for (bone,channel),intervals in changed.items():
        j=0 if channel=='rotations'else 1;keys=getattr(controllers[bone].data,channel).keys
        assert np.array_equal(keys[:,0],grid)
        for first,last in intervals:keys[first]=before[bone][j][first];keys[last]=before[bone][j][last]
    shutil.copytree(source,output);stream.save(output/REL);after=tracks(output/REL)
    assert labels==[(float(t),str(k))for t,k in load(output/REL).root.extra_data.keys]
    for bone,pair in before.items():
        for channel,old,new in zip(('rotations','translations'),pair,after[bone]):
            assert np.array_equal(old[:,0],new[:,0]);keep=np.ones(len(old),bool)
            for first,last in changed.get((bone,channel),[]):keep[first+1:last]=False
            assert np.array_equal(old[keep],new[keep]),(bone,channel)
    worlds={};errors=[];shoulder_errors=[]
    for i,H,target,S in goals:
        if i not in worlds:worlds[i]=rig.world(after,i)
        errors.append(float(norm(worlds[i][H][:3,3]-target)));shoulder_errors.append(float(norm(worlds[i][H.replace('Hand','UpperArm')][:3,3]-S)))
    assert max(clamps)<1e-5,max(clamps);assert max(errors)<1e-4;assert max(shoulder_errors)<1e-4
    report=dict(source=str(source),output=str(output),sections=sections,settle_seconds=settle,prep_lift=prep_lift,return_lateral_duration=return_lateral,open_reference_time=float(grid[open_index]),open_reference_radial_distance=float(norm(free_relative[[0,2],3])),open_reference_finger_distances=finger_reference_distances,max_reach_clamp=max(clamps),max_wrist_error=max(errors),max_shoulder_error=max(shoulder_errors),all_other_channels_and_events_exact=True,source_sha256=sha(source/REL),output_sha256=sha(output/REL),recipe_sha256=sha(Path(__file__)),status='applied')
    manifest=json.loads((source/'weapon-families-import.json').read_text());manifest['outputs'][REL.as_posix()]=report['output_sha256'];manifest['staff']['neutral_gesture']=report
    (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n');(output/'staff-neutral-import.json').write_text(json.dumps(report,indent=2)+'\n');(output/'staff-neutral-targets.json').write_text(json.dumps(receipts,indent=2)+'\n')
    return report
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--input',required=True,type=Path);p.add_argument('--output',required=True,type=Path);p.add_argument('--prep-lift',type=float,default=12.);a=p.parse_args();print(json.dumps(patch(a.input,a.output,prep_lift=a.prep_lift),indent=2))
