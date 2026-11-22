#!/usr/bin/env python3
"""Convert Dark Messiah's movement and sword clips for the native first-person body.

The output contains animation data from the user's own game and stays in the
user's data directory.
"""
import argparse
import json
from pathlib import Path
import sys
import numpy as np

from retarget_kf import cross, norm
from dm_sources import archives, entries, extract, store
from inspect_mdl import load_mdl
from retarget_kf import quat_matrix, matrix_quat

ROOT = Path(__file__).resolve().parents[2]
FPS = 30


class Importer:
    def __init__(self, work, vpk, skeleton, arm_pitch=30, grip_animation=None):
        self.work, self.vpk = work, vpk
        self.models, self.clips, self.provenance = {}, {}, []
        sys.path.insert(0, str(ROOT/'third_party/io_scene_mw/lib'))
        from es3 import nif
        self.nif = nif
        self.grip={}
        if grip_animation:
            original=nif.NiStream();original.load(grip_animation)
            at=next(float(t) for t,label in original.root.extra_data.keys
                    if 'idle1h: start' in label.lower())
            names=[e.string_data for e in original.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
            for name,controller in zip(names,original.root.controllers):
                if name.startswith('Bip01 R Finger'):
                    rows=controller.data.rotations.keys
                    q=np.array([np.interp(at,rows[:,0],rows[:,i]) for i in range(1,5)])
                    self.grip[name]=q/norm(q)
        scene = nif.NiStream(); scene.load(skeleton)
        self.nodes = {n.name:n for n in scene.objects_of_type(nif.NiNode)}
        self.parents = {c.name:n.name for n in self.nodes.values()
                        for c in n.children if isinstance(c,nif.NiNode)}
        self.mapping = json.loads((ROOT/'scripts/animation/dark_messiah_bones.json').read_text())
        self.rest = {}
        def rest(name):
            if name not in self.rest:
                self.rest[name] = (rest(self.parents[name]) if name in self.parents else np.eye(3)) @ self.nodes[name].rotation
            return self.rest[name]
        for name in self.nodes: rest(name)
        self.basis = np.array([[0,-1,0],[1,0,0],[0,0,1.]])
        angle=np.deg2rad(arm_pitch)
        self.arm_lift=np.array([[1,0,0],[0,np.cos(angle),-np.sin(angle)],[0,np.sin(angle),np.cos(angle)]])
        # This known neutral full-body pose has the same lowered-arm stance as
        # Morrowind's normal rest. Reuse it across clips, never zero each attack.
        self.reference = self.globals(self.clip('player_animations','@attack_kick_medium'),0)

    def clip(self, model, name):
        key = model,name
        if key in self.clips: return self.clips[key]
        if model not in self.models:
            dst = self.work/(model+'.mdl')
            if not dst.exists():
                wanted = 'mm/models/npc/player/'+model+'.mdl'
                for archive in reversed(archives(self.vpk)):
                    found = next((e for n,e in entries(archive) if n.lower()==wanted),None)
                    if found is not None:
                        store(dst,extract(archive,found)); break
                else: raise FileNotFoundError(wanted)
            self.models[model] = load_mdl(dst, ROOT/'third_party/SourceIO')
        buf,header,bones,animations = self.models[model]
        anim = next(a for a in animations if a.name==name)
        if anim.animblock_id or int(anim.flags)&4:
            raise ValueError(f'Unsupported external/delta clip: {key}')
        tracks = anim.read_animations(buf,bones)
        result = (bones,tracks,anim)
        self.clips[key] = result
        self.provenance.append({'model':model,'clip':name,'fps':anim.fps,'frames':anim.frame_count})
        return result

    def globals(self, clip, t):
        bones,tracks,anim = clip
        frame = min(int(round(t*anim.fps)),anim.frame_count-1)
        cache = {}
        def get(i):
            b = bones[i]
            if i not in cache:
                q = tracks[b.name]['rot'][min(frame,len(tracks[b.name]['rot'])-1)] if b.name in tracks else b.quat
                cache[i] = (get(b.parent_id) if b.parent_id>=0 else np.eye(3)) @ quat_matrix(q)
            return cache[i]
        return {b.name:get(i) for i,b in enumerate(bones)}

    @staticmethod
    def duration(clip): return max(1/FPS,(clip[2].frame_count-1)/clip[2].fps)

    def pose(self, base, time, arms=None, arms_time=None):
        source = self.globals(base,time)
        arm_targets=set()
        if arms:
            upper = self.globals(arms,time if arms_time is None else arms_time)
            for target,name in self.mapping.items():
                if any(part in target for part in ('Clavicle','UpperArm','Forearm','Hand')) and name in upper:
                    source[name] = upper[name]
                    arm_targets.add(target)
        world, local = {}, {}
        def visit(name):
            if name not in world:
                parent = visit(self.parents[name]) if name in self.parents else np.eye(3)
                src = self.mapping.get(name)
                if src in source and src in self.reference:
                    world[name] = self.basis @ source[src] @ self.reference[src].T @ self.basis.T @ self.rest[name]
                    if name in arm_targets:
                        # Source viewmodel arms sit closer to the eye than the
                        # longer normal Morrowind rig. Calibrate shoulder pitch
                        # while preserving the clip's elbow/wrist articulation.
                        world[name] = self.arm_lift @ world[name]
                    local[name] = matrix_quat(parent.T @ world[name])
                else: world[name] = parent @ self.nodes[name].rotation
            return world[name]
        for name in self.nodes: visit(name)
        return local

    def segment(self, base, arms=None, duration=None, loop=False):
        duration = duration or self.duration(base)
        count = max(2,round(duration*FPS)+1)
        times = np.linspace(0,duration,count)
        poses = [self.pose(base,float(t)*self.duration(base)/duration if loop else float(t)%self.duration(base),arms,
                          (float(t)*self.duration(arms)/duration if loop else min(float(t),self.duration(arms))) if arms else None) for t in times]
        # Preserve the final frame for non-looping clips.
        poses[-1] = self.pose(base,min(duration,self.duration(base)),arms,
                             min(duration,self.duration(arms)) if arms else None)
        return times,poses

    def write(self, out, group, segments, textkeys, root_velocity=None):
        nif = self.nif
        all_times,all_poses = [],[]
        for offset,times,poses in segments:
            all_times.extend(offset+times); all_poses.extend(poses)
        keys = {}
        for name in self.mapping:
            rows = [[t,*p[name]] for t,p in zip(all_times,all_poses) if name in p]
            if not rows: continue
            values = np.asarray(rows,dtype=np.float32)
            for i in range(1,len(values)):
                if np.dot(values[i-1,1:],values[i,1:])<0: values[i,1:]*=-1
            keys[name]=values
        if group.endswith('1h') or group=='weapononehand' or group.startswith('brutal_block'):
            # Static grip calibration for the target hand geometry. Body/arm
            # movement remains the imported Dark Messiah animation.
            for name,q in self.grip.items():
                keys[name]=np.array([[all_times[0],*q],[all_times[-1],*q]],np.float32)
        duration = float(max(all_times))
        if root_velocity is not None:
            q=matrix_quat(self.nodes['Bip01'].rotation)
            keys['Bip01']=np.array([[0,*q],[duration,*q]],np.float32)
        marks=sorted([(float(t),group+': '+label) for t,label in textkeys],key=lambda x:x[0])
        stream=nif.NiStream()
        stream.root=nif.NiSequenceStreamHelper(extra_data=nif.NiTextKeyExtraData(keys=np.array(marks,dtype='<f,O')))
        for name,values in keys.items():
            translation=np.array([[0,*self.nodes[name].translation]],np.float32)
            if name=='Bip01' and root_velocity is not None:
                # Native NPC movement consumes accumulated root translation.
                # These calibrated speeds are not recovered DM constants.
                translation=np.array([[0,*self.nodes[name].translation],
                    [duration,*(self.nodes[name].translation+np.array(root_velocity)*duration)]],np.float32)
            data=nif.NiKeyframeData(rotations=nif.NiRotData(keys=values),
                translations=nif.NiPosData(keys=translation))
            stream.root.extra_datas.append(nif.NiStringExtraData(string_data=name))
            stream.root.controllers.append(nif.NiKeyframeController(data=data,start_time=0.,stop_time=duration,flags=12))
        path=out/(group+'.kf');stream.save(path)
        check=nif.NiStream();check.load(path)
        assert len(list(check.root.controllers))==len(keys)
        for ctrl,expected in zip(check.root.controllers,keys.values()):
            np.testing.assert_allclose(ctrl.data.rotations.keys,expected,atol=1e-6)
        print(f'{group}: {duration:.3f}s, {len(keys)} bones, {len(marks)} native events')

    def loop(self,out,group,body_name,arms_name=None,root_velocity=None):
        base=self.clip('player_animations',body_name)
        arms=self.clip('first_animations_basic',arms_name) if arms_name else None
        duration=max(self.duration(base),self.duration(arms) if arms else 0)
        times,poses=self.segment(base,arms,duration,loop=True)
        self.write(out,group,[(0,times,poses)],[(0,'start'),(0,'loop start'),(duration,'loop stop'),(duration,'stop')],root_velocity=root_velocity)

    def weapon(self,out,npc=False):
        base=self.clip('player_animations','@Combat_Idle')
        segments,marks,offset=[],[(0,'start')],0.
        for phase,clipname in [('equip','anm_sword_noshield_quickdraw'),('unequip','anm_sword_noshield_undraw')]:
            arm=self.clip('first_animations_basic',clipname) if not npc else None
            body=base if not npc else self.clip('player_animations','anm_weapon_draw' if phase=='equip' else 'anm_weapon_undraw')
            d=self.duration(arm or body)
            times,poses=self.segment(body,arm,d)
            segments.append((offset,times,poses))
            marks.extend([(offset,phase+' start'),(offset+d*.4,phase+(' attach' if phase=='equip' else ' detach')),(offset+d,phase+' stop')])
            offset+=d+.05
        # Keep the established attacks in place and append the authored _R arc.
        attacks=[('chop','_T'),('slash','_L'),('thrust','_B')]
        if not npc:
            attacks.append(('slashright','_R'))
        for attack,suffix in attacks:
            prefix='anm_bigslash_sword_noshield'+suffix
            model='player_animations' if npc else 'first_animations_sword'
            wind=self.clip(model,prefix+'_in')
            release=self.clip(model,prefix+'_out')
            windtime=self.duration(wind);reltime=self.duration(release)
            times,poses=self.segment(wind if npc else base,None if npc else wind,windtime)
            segments.append((offset,times,poses))
            # Tiny gap avoids duplicate quaternion timestamps at splice boundaries.
            times,poses=self.segment(release if npc else base,None if npc else release,reltime)
            segments.append((offset+windtime+.001,times,poses))
            hit=offset+windtime+min(.30,reltime*.40)
            stop=offset+windtime+.001+reltime
            marks.extend([(offset,attack+' start'),(offset+.15,attack+' min attack'),
                          (offset+windtime,attack+' max attack'),
                          (offset+windtime+.08,attack+' min hit'),(hit,attack+' hit')])
            for strength in ('small','medium','large'):
                marks.extend([(hit+.001,attack+' '+strength+' follow start'),(stop,attack+' '+strength+' follow stop')])
            offset=stop+.05
        marks.append((offset-.05,'stop'))
        self.write(out,'weapononehand',segments,marks)

    def jump(self,out,group,sword=False):
        airborne=self.clip('player_animations','@jump_idle')
        landing=self.clip('player_animations','@jump_out_idle')
        arms=self.clip('first_animations_basic','@jump_idle_sword_noshield') if sword else None
        recover=self.clip('first_animations_basic','@jump_out_sword_noshield') if sword else None
        d=self.duration(airborne);end=d+.001+self.duration(landing)
        t1,p1=self.segment(airborne,arms,d,loop=True)
        t2,p2=self.segment(landing,recover)
        self.write(out,group,[(0,t1,p1),(d+.001,t2,p2)],[(0,'start'),(0,'loop start'),(d,'loop stop'),(end,'stop')])

    def sword_kick(self,out):
        body=self.clip('player_animations','@attack_kick_medium')
        arms=self.clip('first_animations_basic','anm_sword_noshield_kick_medium')
        times,poses=self.segment(body,arms)
        self.write(out,'brutal_kick1h',[(0,times,poses)],[(0,'start'),(.375,'hit'),(times[-1],'stop')])

    def shield_block(self,out,arm_pitch):
        # Dark Messiah's RMB shield parry: raise, held loop, lower, plus the
        # three impact reactions played when a blow lands on the raised shield.
        # The Morrowind shield hangs further from the eye than DM's, so the
        # shoulder calibration is separate from the sword clips.
        angle=np.deg2rad(arm_pitch);saved=self.arm_lift
        self.arm_lift=np.array([[1,0,0],[0,np.cos(angle),-np.sin(angle)],[0,np.sin(angle),np.cos(angle)]])
        base=self.clip('player_animations','@Combat_Idle')
        raise_,hold,lower=(self.clip('first_animations_shield','anm_parry_sword_shield_'+p) for p in ('in','idle','out'))
        # Stretch the tiny held-arm loop over the whole torso idle cycle so both loop seamlessly.
        d_in,d_hold,d_out=self.duration(raise_),self.duration(base),self.duration(lower)
        t1,p1=self.segment(base,raise_,d_in)
        t2,p2=self.segment(base,hold,d_hold,loop=True)
        t3,p3=self.segment(base,lower,d_out)
        hold_at=d_in+.001;lower_at=hold_at+d_hold+.001;end=lower_at+d_out
        self.write(out,'brutal_block',[(0,t1,p1),(hold_at,t2,p2),(lower_at,t3,p3)],
            [(0,'start'),(hold_at,'loop start'),(hold_at+d_hold,'loop stop'),(lower_at,'lower start'),(end,'stop')])
        for index,variant in enumerate('ABC',1):
            impact=self.clip('first_animations_shield','anm_parry_sword_shield_'+variant)
            times,poses=self.segment(base,impact,self.duration(impact))
            self.write(out,f'brutal_blockhit{index}',[(0,times,poses)],[(0,'start'),(times[-1],'stop')])
        self.arm_lift=saved


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root',type=Path,required=True)
    ap.add_argument('--work-dir',type=Path,required=True)
    ap.add_argument('--skeleton',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--npc-output',type=Path,help='Separate data root for humanoid NPC animation overrides')
    ap.add_argument('--arm-pitch',type=float,default=30,help='First-person shoulder elevation calibration in degrees')
    ap.add_argument('--block-arm-pitch',type=float,default=10,help='Shoulder elevation calibration for the shield block clips')
    ap.add_argument('--grip-animation',type=Path,help='Morrowind\'s xbase_anim.kf for target-rig sword grip calibration')
    a=ap.parse_args();a.work_dir.mkdir(parents=True,exist_ok=True)
    out=a.output/'Animations/brutal_firstperson';out.mkdir(parents=True,exist_ok=True)
    grip=a.grip_animation or a.work_dir/'xbase_anim.kf'
    if not grip.exists():
        raise FileNotFoundError('Extract meshes/xbase_anim.kf from your Morrowind.bsa into --work-dir, or pass --grip-animation')
    imp=Importer(a.work_dir,a.vpk_root,a.skeleton,a.arm_pitch,grip)
    imp.loop(out,'idle','@idle_middle')
    imp.loop(out,'idle1h','@Combat_Idle','anm_sword_noshield_idle')
    imp.loop(out,'idlesneak','@Crouch_Idle')
    for mode,src in [('walk','Walk'),('run','Run'),('sneak','Crouch')]:
        for direction,compass in [('forward','N'),('back','S'),('left','W'),('right','E')]:
            clip='a_'+src+compass
            imp.loop(out,mode+direction,clip)
            imp.loop(out,mode+direction+'1h',clip,'@run_sword' if mode=='run' else '@walk_sword')
    imp.weapon(out)
    imp.sword_kick(out)
    imp.shield_block(out,a.block_arm_pitch)
    imp.jump(out,'jump')
    imp.jump(out,'jump1h',True)
    if a.npc_output:
        # NPCs need genuine guard clips and their own neutral reference. Keep
        # this command for both, but never regenerate player poses for NPCs.
        import subprocess
        subprocess.run([sys.executable, str(ROOT/'scripts/animation/import_npc.py'),
            '--vpk-root', str(a.vpk_root), '--work-dir', str(a.work_dir/'npc'),
            '--skeleton', str(a.skeleton), '--grip-animation', str(grip),
            '--output', str(a.npc_output)], check=True)
    (a.output/'first-person-import.json').write_text(json.dumps({'converter':'import_first_person.py','clips':imp.provenance,
        'notes':'Derived from the user\'s own game files. Hit timestamps are chosen integration markers, not decoded original game events.'},indent=2)+'\n')


if __name__=='__main__': main()
