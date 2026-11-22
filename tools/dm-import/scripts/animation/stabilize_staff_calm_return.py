#!/usr/bin/env python3
"""One synchronized polearm return with anchored shoulders and a single catch.

The left wrist moves directly to its end pose while the right hand brings the
shaft grip toward it. Both motions share one eased progress. There is no radial
clearance hump, preparatory wave, or independently retained source recovery.
"""
import argparse, hashlib, json, shutil
from pathlib import Path
import numpy as np
from retarget_kf import norm
from audit_staff_mesh_grip import Rig, slerp
from audit_viewmodel_continuity import tracks
from retarget_kf import matrix_quat, quat_matrix
from import_staff_native_grasp import swing

SIDES = ('L', 'R')
def name(side, bone): return f'Bip01 {side} {bone}'
ARMS = {name(s, b) for s in SIDES for b in ('UpperArm','Forearm','Hand')}
FINGERS = {name(s,f'Finger{i}{suffix}') for s in SIDES for i in range(5) for suffix in ('','1','2')}
CHANGED = ARMS | FINGERS

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def smooth(x):
 x=float(np.clip(x,0.,1.)); return x*x*(3.-2.*x)
def rotmix(a,b,w): return quat_matrix(slerp(matrix_quat(a),matrix_quat(b),w)[[1,2,3,0]])
def nif_stream(path):
 from es3 import nif
 s=nif.NiStream(); s.load(path); return s

def patch(source, output, rotation_power=1.5, close_distance=6.):
 if output.exists(): raise ValueError('Fresh output required')
 rel=Path('Animations/brutal_fp_staff/weapononehand.kf')
 rig=Rig(source/'meshes/brutal_fp/xbase_anim.1st.nif'); before=tracks(source/rel)
 grid=before[name('L','Hand')][0][:,0]; stream=nif_stream(source/rel)
 labels=[(float(t),str(k)) for t,k in stream.root.extra_data.keys]
 marks={k.removeprefix('weapononehand: '):t for t,k in labels}
 first=int(np.searchsorted(grid,marks['slash hit2']+.05)); last=int(np.argmin(abs(grid-marks['slash large follow stop'])))
 start,stop=float(grid[first]),float(grid[last]); A=rig.world(before,first); B=rig.world(before,last)
 LH,RH=name('L','Hand'),name('R','Hand')
 grip=np.linalg.inv(B[RH])@B[LH]
 grip0=(A[RH]@grip)[:3,3]; lh0=A[LH][:3,3]; lh1=B[LH][:3,3]; gap0=float(norm(grip0-lh0))
 from es3 import nif
 names=[e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
 controllers=dict(zip(names,stream.root.controllers)); clamps=[]; endpoint_errors=[]; rows=[]; previous_q={}
 for i in range(first,last+1):
  t=float(grid[i]); w=smooth((t-start)/(stop-start)); world=rig.world(before,i)
  left=(1-w)*lh0+w*lh1; g=(1-w)*grip0+w*lh1
  RR=rotmix(A[RH][:3,:3],B[RH][:3,:3],w**rotation_power)
  right=g-RR@grip[:3,3]
  rotations={'L':rotmix(A[LH][:3,:3],B[LH][:3,:3],w),'R':RR}
  for side,target in [('L',left),('R',right)]:
   U,F,H,C=[name(side,b) for b in ('UpperArm','Forearm','Hand','Clavicle')]
   S=world[U][:3,3]; l1=float(norm(before[F][1][i,1:])); l2=float(norm(before[H][1][i,1:]))
   delta=target-S; distance=float(norm(delta)); axis=delta/max(distance,1e-12)
   bounded=float(np.clip(distance,abs(l1-l2)+1e-5,l1+l2-1e-5)); clamps.append(abs(distance-bounded)); target=S+axis*bounded
   # Endpoint-only elbow reference: no per-frame flourish/twist leaks in.
   elbow_ref=(1-w)*A[F][:3,3]+w*B[F][:3,3]
   bend=elbow_ref-S; bend-=axis*np.dot(bend,axis)
   if norm(bend)<1e-7: raise ValueError('Degenerate elbow reference')
   bend/=norm(bend)
   along=(l1*l1-l2*l2+bounded*bounded)/(2*bounded); height=np.sqrt(max(0.,l1*l1-along*along))
   elbow=S+axis*along+bend*height
   upper_ref=rotmix(A[U][:3,:3],B[U][:3,:3],w)
   Ru=swing(upper_ref@before[F][1][i,1:],elbow-S)@upper_ref
   hand_local=rotmix(A[F][:3,:3].T@A[H][:3,:3],B[F][:3,:3].T@B[H][:3,:3],w)
   fore_ref=rotations[side]@hand_local.T
   Rf=swing(fore_ref@before[H][1][i,1:],target-elbow)@fore_ref
   local={U:world[C][:3,:3].T@Ru,F:Ru.T@Rf,H:Rf.T@rotations[side]}
   for bone,R in local.items():
    q=matrix_quat(R)
    if bone in previous_q and np.dot(q,previous_q[bone])<0: q=-q
    previous_q[bone]=q
    if i in (first,last): endpoint_errors.append(float(norm(quat_matrix(before[bone][0][i,[2,3,4,1]])-R)))
    controllers[bone].data.rotations.keys[i,1:]=q
  gap=(1-w)*gap0; closure=smooth(1-gap/close_distance)
  for bone in FINGERS:
   # Right hand stays seated; left closes only during its one near-shaft arrival.
   f=closure if bone.startswith('Bip01 L ') else w
   controllers[bone].data.rotations.keys[i,1:]=slerp(before[bone][0][first,1:],before[bone][0][last,1:],f)
   controllers[bone].data.translations.keys[i,1:]=(1-f)*before[bone][1][first,1:]+f*before[bone][1][last,1:]
  rows.append(dict(time=t,progress=w,gap=gap,finger_close=closure,left=left.tolist(),right=right.tolist()))
 for bone in CHANGED:
  for channel,j in [('rotations',0),('translations',1)]:
   keys=getattr(controllers[bone].data,channel).keys; keys[first]=before[bone][j][first]; keys[last]=before[bone][j][last]
 shutil.copytree(source,output); stream.save(output/rel); after=tracks(output/rel)
 assert labels==[(float(t),str(k)) for t,k in nif_stream(output/rel).root.extra_data.keys]
 for bone,pair in before.items():
  for old,new in zip(pair,after[bone]):
   assert np.array_equal(old[:,0],new[:,0]),bone+' clocks'
   mask=np.ones(len(old),bool)
   if bone in CHANGED: mask[first:last+1]=False
   assert np.array_equal(old[mask],new[mask]),bone+' retained data'
 for side in SIDES:
  C=name(side,'Clavicle'); assert all(np.array_equal(a,b) for a,b in zip(before[C],after[C])),C
  for bone in ('UpperArm','Forearm','Hand'):
   k=name(side,bone); assert np.array_equal(before[k][1],after[k][1]),k+' origin'
 assert all(np.array_equal(a,b) for a,b in zip(before['Weapon Bone'],after['Weapon Bone']))
 max_shoulder=max_length=0.
 for i in range(first,last+1):
  old=rig.world(before,i); new=rig.world(after,i)
  for side in SIDES:
   U,F,H=[name(side,b) for b in ('UpperArm','Forearm','Hand')]
   max_shoulder=max(max_shoulder,float(norm(old[U][:3,3]-new[U][:3,3])))
   for a,b in [(U,F),(F,H)]: max_length=max(max_length,abs(float(norm(old[b][:3,3]-old[a][:3,3])-norm(new[b][:3,3]-new[a][:3,3]))))
 report=dict(source=str(source.resolve()),output=str(output.resolve()),mode='single-synchronized-anchored-return',file=rel.as_posix(),start=start,stop=stop,
  changed_tracks=sorted(CHANGED),rotation_power=rotation_power,close_distance=close_distance,initial_gap=gap0,
  clavicles_byte_identical=True,bone_origin_translations_byte_identical=True,weapon_local_mount_byte_identical=True,
  max_shoulder_shift=max_shoulder,max_bone_length_error=max_length,max_reach_clamp=max(clamps),max_endpoint_matrix_error=max(endpoint_errors),
  damage_poses_keys_clocks_unchanged=True,source_sha256=sha(source/rel),output_sha256=sha(output/rel),recipe_sha256=sha(Path(__file__)),
  status='applied')
 manifest=json.load(open(source/'weapon-families-import.json')); manifest['outputs'][rel.as_posix()]=report['output_sha256']; manifest['staff']['calm_return']=report
 (output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n')
 (output/'staff-calm-return-import.json').write_text(json.dumps(report,indent=2)+'\n')
 (output/'staff-calm-return-targets.json').write_text(json.dumps(rows,indent=2)+'\n')
 report['manifest_sha256']=sha(output/'weapon-families-import.json'); return report

def main():
 p=argparse.ArgumentParser(description=__doc__); p.add_argument('--input',required=True,type=Path); p.add_argument('--output',required=True,type=Path)
 p.add_argument('--rotation-power',type=float,default=1.5); p.add_argument('--close-distance',type=float,default=6.); a=p.parse_args()
 print(json.dumps(patch(a.input,a.output,a.rotation_power,a.close_distance),indent=2))
if __name__=='__main__':main()
