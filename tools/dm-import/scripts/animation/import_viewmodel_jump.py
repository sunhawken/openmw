#!/usr/bin/env python3
"""Import Dark Messiah's jump motion into the calibrated first-person hand frames.

The source jump wrist motion is applied relative to its own idle, anchored to the
native idle. This keeps the physical hand/finger/item calibration. Polearms move
both closed grips together; shoulders and arm lengths remain anchored. Only new
jump and stationary sneak aliases are emitted; existing attack assets stay exact.
"""
import argparse,copy,hashlib,json,shutil
from pathlib import Path
import numpy as np
from retarget_kf import norm
from import_viewmodel import Viewmodel,BASIS,FPS
from audit_staff_mesh_grip import Rig,slerp,matrix
from audit_viewmodel_continuity import tracks
from retarget_kf import quat_matrix,matrix_quat
from stabilize_staff_left_windup import load,smooth,mix,unit,swing

SETS={
 'brutal_fp':('sword_noshield','anm_sword_noshield_idle'),
 'brutal_fp_shield':('sword_shield','anm_shield_idle'),
 'brutal_fp_staff':('staff','anm_staff_idle'),
 'brutal_fp_dagger':('daggers','anm_daggers_idle'),
}
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()

def M(qp):q,p=qp;return matrix(quat_matrix(q[[1,2,3,0]]),p)

def sample_channels(channels,time):
 result={}
 for name,(rot,trans)in channels.items():
  i=int(np.clip(np.searchsorted(rot[:,0],time,side='right')-1,0,len(rot)-1));j=min(i+1,len(rot)-1);span=rot[j,0]-rot[i,0];w=float(np.clip((time-rot[i,0])/span,0,1))if span else 0
  q=slerp(rot[i,1:],rot[j,1:],w);p=np.array([np.interp(time,trans[:,0],trans[:,k])for k in range(1,4)]);result[name]=(q,p)
 return result

class Source(Viewmodel):
 def __init__(self,work,vpk):
  self.work,self.vpk=work,vpk;self.models={};self.provenance=[];self.sequences={};self.cached={}
 def dm_clip(self,model,name):
  key=(model,name)
  if key not in self.cached:self.cached[key]=super().dm_clip(model,name)
  return self.cached[key]

def alias(src,dst,old,new):
 stream=load(src);stream.root.extra_data.keys=np.array([(float(t),str(k).replace(old+':',new+':',1))for t,k in stream.root.extra_data.keys],dtype='<f,O');stream.save(dst)
 y=src.with_suffix('.yaml')
 if y.exists():dst.with_suffix('.yaml').write_text(y.read_text().replace(old,new))

def bake(source,output,body_source,body_output,work,vpk):
 assert not output.exists() and not body_output.exists(),'Fresh outputs required'
 shutil.copytree(source,output);shutil.copytree(body_source,body_output)
 dm=Source(work,vpk);rig=Rig(source/'meshes/brutal_fp/xbase_anim.1st.nif');scale=float(json.loads((source/'viewmodel-import.json').read_text())['scale']);reports=[]
 requests=[(folder,'jump1h',suffix,idle,False)for folder,(suffix,idle)in SETS.items()]+[('brutal_fp','jump',None,'@idle_middle',True)]
 for folder,group,suffix,idle,unarmed in requests:
  idle_path=source/'Animations'/folder/('idle.kf'if unarmed else'idle1h.kf');C=tracks(idle_path);base=sample_channels(C,0);static={n:M(v)for n,v in base.items()};K={n:(np.array([[0,*v[0]]]),np.array([[0,*v[1]]]))for n,v in base.items()};world=rig.world(K,0)
  model='player_animations'if unarmed else'first_animations_basic';idle_dm=dm.dm_world(model,idle,0)
  entry=0 if unarmed else dm.duration(model,'@jump_in_'+suffix)
  air_clip='@jump_idle'if unarmed else'@jump_idle_'+suffix;out_clip='@jump_out_idle'if unarmed else'@jump_out_'+suffix
  air_period=dm.duration(model,air_clip);landing=dm.duration(model,out_clip);bridge=.15 if entry else .2
  loop_start=entry+bridge;loop_stop=loop_start+air_period;stop=loop_stop+landing+.2
  ticks=np.unique(np.concatenate([np.linspace(0,entry,max(2,round(entry*FPS)+1))if entry else [0],np.linspace(entry,loop_start,max(2,round(bridge*FPS)+1)),np.linspace(loop_start,loop_stop,max(2,round(air_period*FPS)+1)),np.linspace(loop_stop,loop_stop+landing,max(2,round(landing*FPS)+1)),np.linspace(loop_stop+landing,stop,7)]))
  # Native mount remains the calibrated physical local item transform. Dagger idle
  # first sample already equals compensating frame * settled primitive endpoint.
  targets=[];clamps=[];poses=[]
  def sample_source(t):
   if t<entry:return dm.dm_world(model,'@jump_in_'+suffix,t)
   if t<loop_start:
    first=dm.dm_world(model,'@jump_in_'+suffix,entry)if entry else idle_dm
    last=dm.dm_world(model,air_clip,0);w=smooth((t-entry)/bridge)
    return {n:matrix(mix(first[n][:3,:3],last[n][:3,:3],w),(1-w)*first[n][:3,3]+w*last[n][:3,3])for n in first.keys()&last.keys()}
   if t<loop_stop:return dm.dm_world(model,air_clip,t-loop_start)
   return dm.dm_world(model,out_clip,min(t-loop_stop,landing))
  raw_samples=[sample_source(float(t))for t in ticks]
  def wrist_goals(raw,gain):
   goals,deltas={},{}
   for side in 'LR':
    b='SK_'+side+'_Wrist';n='Bip01 '+side+' Hand';R=BASIS@raw[b][:3,:3]@idle_dm[b][:3,:3].T@BASIS.T
    R=mix(np.eye(3),R,gain);p=world[n][:3,3]+gain*scale*BASIS@(raw[b][:3,3]-idle_dm[b][:3,3]);goals[side]=matrix(R@world[n][:3,:3],p);deltas[side]=R
   if folder=='brutal_fp_staff':
    nR,nL='Bip01 R Hand','Bip01 L Hand';relative=np.linalg.inv(world[nR])@world[nL];goals['L']=goals['R']@relative
   return goals,deltas
  def feasible(gain):
   for raw in raw_samples:
    goals,_=wrist_goals(raw,gain)
    for side in 'LR':
     U,F,H=['Bip01 '+side+' '+x for x in ['UpperArm','Forearm','Hand']];l1=norm(static[F][:3,3]);l2=norm(static[H][:3,3]);d=norm(goals[side][:3,3]-world[U][:3,3])
     if d<abs(l1-l2)+.05 or d>l1+l2-.25:return False
   return True
  gain=next((float(g)for g in np.linspace(1,0,1001)if feasible(float(g))),None)
  assert gain is not None,'Native idle itself exceeds safe reach'
  for t,raw in zip(ticks,raw_samples):
   pose={n:x.copy()for n,x in static.items()};goals,deltas=wrist_goals(raw,gain)
   tail=smooth((float(t)-(loop_stop+landing))/.2)
   if tail>0:
    for side in'LR':
     H='Bip01 '+side+' Hand';goals[side]=matrix(mix(goals[side][:3,:3],world[H][:3,:3],tail),(1-tail)*goals[side][:3,3]+tail*world[H][:3,3]);deltas[side]=mix(deltas[side],np.eye(3),tail)
   for side in'LR':
    Cn,U,F,H=['Bip01 '+side+' '+x for x in ['Clavicle','UpperArm','Forearm','Hand']];S=world[U][:3,3];target=goals[side][:3,3];l1=norm(static[F][:3,3]);l2=norm(static[H][:3,3]);delta=target-S;distance=norm(delta);bounded=float(np.clip(distance,abs(l1-l2)+1e-5,l1+l2-1e-5));clamps.append(float(abs(distance-bounded)));axis=unit(delta);target=S+axis*bounded
    pole=deltas[side]@(world[F][:3,3]-S);pole=unit(pole-axis*np.dot(pole,axis));along=(l1*l1-l2*l2+bounded*bounded)/(2*bounded);elbow=S+axis*along+pole*np.sqrt(max(0,l1*l1-along*along))
    upper=swing(unit(world[F][:3,3]-S),unit(elbow-S))@world[U][:3,:3];fore=swing(unit(world[H][:3,3]-world[F][:3,3]),unit(target-elbow))@world[F][:3,:3]
    pose[U][:3,:3]=world[Cn][:3,:3].T@upper;pose[F][:3,:3]=upper.T@fore;pose[H][:3,:3]=fore.T@goals[side][:3,:3];targets.append((float(t),H,target.copy()))
   poses.append(pose)
  assert max(clamps)<1e-5,(folder,max(clamps))
  # Close the air seam exactly; this copies only its endpoint sample.
  ia=int(np.argmin(abs(ticks-loop_start)));ib=int(np.argmin(abs(ticks-loop_stop)))
  poses[ib]=copy.deepcopy(poses[ia]);poses[-1]=copy.deepcopy(static)
  template=load(idle_path);names=[e.string_data for e in template.root.extra_datas if e.__class__.__name__=='NiStringExtraData'];controllers=dict(zip(names,template.root.controllers));assert set(names)==set(static)
  for n,c in controllers.items():
   q=np.array([matrix_quat(x[n][:3,:3])for x in poses]);
   for i in range(1,len(q)):
    if q[i]@q[i-1]<0:q[i]*=-1
   c.data.rotations.keys=np.column_stack([ticks,q]).astype(np.float32);c.data.translations.keys=np.column_stack([ticks,[x[n][:3,3]for x in poses]]).astype(np.float32)
   if c.data.scales is not None and len(c.data.scales.keys):c.data.scales.keys=np.array([[0,c.data.scales.keys[0,1]],[stop,c.data.scales.keys[0,1]]],dtype=np.float32)
   c.start_time=0;c.stop_time=stop
  marks=[(0,'start'),(loop_start,'loop start'),(loop_stop,'loop stop'),(stop,'stop')];template.root.extra_data.keys=np.array([(t,group+': '+k)for t,k in marks],dtype='<f,O')
  primitive_report=None
  if folder=='brutal_fp_dagger':
   original=load(idle_path);oldmarks=[(float(t),str(k))for t,k in original.root.extra_data.keys];primitive_marks=[(t,k)for t,k in oldmarks if ': dagger primitive 'in k];assert primitive_marks
   old_first=min(t for t,k in primitive_marks);new_first=stop+1.;shift=new_first-old_first
   primitive_report=dict(source='idle1h independent item tables',old_first=old_first,new_first=new_first,commands='none: retain current independent item state',arm_frame='idle compensating frames')
   labels=[(t,group+': '+k)for t,k in marks]+[(0,group+': dagger arm frame')]+[(t+shift,k.replace('idle1h:',group+':',1))for t,k in primitive_marks]
   template.root.extra_data.keys=np.array(sorted(labels,key=lambda x:x[0]),dtype='<f,O')
   oldnames=[e.string_data for e in original.root.extra_datas if e.__class__.__name__=='NiStringExtraData'];oldcontrollers=dict(zip(oldnames,original.root.controllers))
   for n in ['Weapon Bone','Brutal Left Weapon']:
    for field in ['rotations','translations']:
     oldkeys=getattr(oldcontrollers[n].data,field).keys;extra=oldkeys[oldkeys[:,0]>=old_first-1e-5].copy();extra[:,0]+=shift;keys=getattr(controllers[n].data,field).keys;getattr(controllers[n].data,field).keys=np.concatenate([keys,extra])
   for c in controllers.values():c.stop_time=max(t for t,k in labels)
  dst=output/'Animations'/folder/(group+'.kf');template.save(dst)
  dst.with_suffix('.yaml').write_text("blending_rules:\n"+''.join("  - from: '*'\n    to: '"+group+':'+k+"'\n    easing: linear\n    duration: 0.2\n    clock: simulation\n"for k in ['start','loop stop'])+"  - from: ''\n    to: '"+group+"'\n    easing: linear\n    duration: 0\n")
  reports.append(dict(folder=folder,group=group,source_model=model,source_clips=([('@jump_in_'+suffix)]if suffix else[])+[air_clip,out_clip],reference_idle=idle,mode='source-wrist-motion-relative-to-idle; anchored native shoulders and physical mounts',marks=marks,max_reach_clamp=max(clamps),constant_source_motion_gain=gain,dagger_item_metadata=primitive_report,local_origins_fingers_mount_camera_retained=True,sha256=sha(dst)))
 # Retime existing imported body jump poses to the primary markers. The source
 # airborne/landing curves stay DM data; the body has no visible first-person arms.
 body_reports=[]
 for group in ['jump','jump1h']:
  primary=next(r for r in reports if r['group']==group);marks=dict((k,t)for t,k in primary['marks']);a,b,end=[marks[k]for k in ['loop start','loop stop','stop']]
  oldpath=body_source/'Animations/brutal_firstperson'/(group+'.kf');old=tracks(oldpath);oldmarks={str(k).split(': ',1)[1]:float(t)for t,k in load(oldpath).root.extra_data.keys};oldair=oldmarks['loop stop'];oldend=oldmarks['stop'];idlebody=tracks(body_source/'Animations/brutal_firstperson'/('idle.kf'if group=='jump'else'idle1h.kf'));rest=sample_channels(idlebody,0)
  grid=np.unique(np.r_[np.linspace(0,a,max(2,round(a*FPS)+1)),np.linspace(a,b,max(2,round((b-a)*FPS)+1)),np.linspace(b,end,max(2,round((end-b)*FPS)+1))]);samples=[]
  for t in grid:
   if t<a:
    air=sample_channels(old,0);w=smooth(float(t)/min(.2,a));sample={}
    from audit_staff_mesh_grip import slerp
    for n,(q,p)in air.items():
     rq,rp=rest.get(n,(q,p));sample[n]=(slerp(rq,q,w),(1-w)*rp+w*p)
   elif t<=b:sample=sample_channels(old,(float(t)-a)/(b-a)*oldair)
   else:sample=sample_channels(old,oldair+(float(t)-b)/(end-b)*(oldend-oldair))
   samples.append(sample)
  template=load(oldpath);names=[e.string_data for e in template.root.extra_datas if e.__class__.__name__=='NiStringExtraData']
  for n,c in zip(names,template.root.controllers):
   Q=np.array([x[n][0]for x in samples]);
   for i in range(1,len(Q)):
    if Q[i]@Q[i-1]<0:Q[i]*=-1
   c.data.rotations.keys=np.column_stack([grid,Q]).astype(np.float32);c.data.translations.keys=np.column_stack([grid,[x[n][1]for x in samples]]).astype(np.float32);c.start_time=0;c.stop_time=end
  template.root.extra_data.keys=np.array([(t,group+': '+k)for t,k in primary['marks']],dtype='<f,O');dest=body_output/'Animations/brutal_firstperson'/(group+'.kf');template.save(dest);body_reports.append(dict(group=group,old_marks=oldmarks,new_marks=primary['marks'],source_sha256=sha(oldpath),output_sha256=sha(dest)))
 # Arm aliases activate the already imported standing crouch body, with stance-specific hands.
 for folder in SETS:
  alias(source/'Animations'/folder/'idle1h.kf',output/'Animations'/folder/'idlesneak1h.kf','idle1h','idlesneak1h')
 alias(source/'Animations/brutal_fp/idle.kf',output/'Animations/brutal_fp/idlesneak.kf','idle','idlesneak')
 alias(body_source/'Animations/brutal_firstperson/idlesneak.kf',body_output/'Animations/brutal_firstperson/idlesneak1h.kf','idlesneak','idlesneak1h')
 manifest=json.loads((output/'weapon-families-import.json').read_text())
 for path in output.rglob('*'):
  if path.suffix in('.kf','.yaml'):manifest['outputs'][path.relative_to(output).as_posix()]=sha(path)
 report=dict(source=str(source),output=str(output),body_source=str(body_source),body_output=str(body_output),motion_scale=scale,jumps=reports,source_clips=dm.provenance,body_retiming=body_reports,existing_files_unchanged=all(sha(p)==sha(output/p.relative_to(source))for p in source.rglob('*')if p.is_file()and p.name!='weapon-families-import.json'),sneak='new primary idlesneak + idlesneak1h aliases, DM bodyidlesneak1h alias; native stance selector required')
 assert report['existing_files_unchanged'];manifest['ordinary_jump_import']=report;(output/'weapon-families-import.json').write_text(json.dumps(manifest,indent=1)+'\n');(output/'ordinary-jump-import.json').write_text(json.dumps(report,indent=2)+'\n');(body_output/'stationary-sneak-import.json').write_text(json.dumps(dict(source='Existing DM @Crouch_Idle idlesneak alias',armed_alias='Animations/brutal_firstperson/idlesneak1h.kf'),indent=2)+'\n');return report

if __name__=='__main__':
 p=argparse.ArgumentParser(description=__doc__)
 for n in['input','output','body-input','body-output','work-dir','vpk-root']:p.add_argument('--'+n,type=Path,required=True)
 a=p.parse_args();print(json.dumps(bake(a.input,a.output,a.body_input,a.body_output,a.work_dir,a.vpk_root),indent=2))
