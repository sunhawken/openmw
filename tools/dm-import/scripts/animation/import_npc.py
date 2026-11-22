#!/usr/bin/env python3
"""Import Dark Messiah's guard animation clips for humanoid NPCs.

The output contains data from the user's game. Player animation is not touched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import numpy as np

from retarget_kf import cross, norm

from dm_sources import archives, entries, extract, store
from import_first_person import Importer, ROOT, FPS
from inspect_mdl import load_mdl
from retarget_kf import quat_matrix, matrix_quat
from import_viewmodel import frame, to4, SIDES


def sample_quaternion(values, frame):
    """Shortest-arc interpolation instead of quantizing 24fps to 30fps."""
    lo = min(int(frame), len(values)-1)
    hi = min(lo+1, len(values)-1)
    t = min(1., max(0., frame-lo))
    a = np.asarray(values[lo], dtype=float); a /= norm(a)
    b = np.asarray(values[hi], dtype=float); b /= norm(b)
    dot = float(np.dot(a,b))
    if dot < 0: b = -b; dot = -dot
    if dot > .9995:
        q = a*(1-t)+b*t
        return q/norm(q)
    angle = np.arccos(np.clip(dot,-1,1))
    return (np.sin((1-t)*angle)*a+np.sin(t*angle)*b)/np.sin(angle)


class GuardImporter(Importer):
    def __init__(self, work, vpk, skeleton, grip):
        super().__init__(work, vpk, skeleton, arm_pitch=0, grip_animation=grip)
        original=self.nif.NiStream();original.load(grip)
        shield_at=next(float(t) for t,label in original.root.extra_data.keys if str(label).strip().lower()=='shield: block hit')
        names=[e.string_data for e in original.root.extra_datas if isinstance(e,self.nif.NiStringExtraData)]
        for name,controller in zip(names,original.root.controllers):
            if name.startswith('Bip01 L Finger'):
                rows=controller.data.rotations.keys
                q=np.array([np.interp(shield_at,rows[:,0],rows[:,j]) for j in range(1,5)])
                self.grip[name]=q/norm(q)
        path = work/'npc_guard_combat_animations.mdl'
        if not path.exists():
            wanted = 'mm/models/npc/guard/npc_guard_combat_animations.mdl'
            for archive in reversed(archives(vpk)):
                entry = next((e for n,e in entries(archive) if n.lower()==wanted), None)
                if entry is not None:
                    store(path,extract(archive,entry)); break
            else: raise FileNotFoundError(wanted)
        self.models['guard'] = load_mdl(path, ROOT/'third_party/SourceIO')
        self.provenance = []  # The inherited player's reference is not used in NPC output.
        # A standing guard reference, not the player's already flexed kick pose.
        self.neutral = self.clip('guard','@idle_0')
        self.reference = self.globals(self.neutral,0)
        self.reference_height = self.root_height(self.neutral,0)
        def rest_position(name):
            node=self.nodes[name]
            if name not in self.parents: return node.translation
            parent=self.parents[name]
            return rest_position(parent)+self.rest[parent]@node.translation
        self.height_scale = float(rest_position('Bip01 Pelvis')[2])/self.reference_height
        self.sequences = {}
        from SourceIO.library.models.mdl.structs.sequence import StudioSequence
        buf,header,bones,animations=self.models['guard']
        buf.seek(header.local_sequence_offset)
        for _ in range(header.local_sequence_count):
            seq=StudioSequence.from_buffer(buf,header.version)
            saved=buf.tell();buf.seek(seq._entry_offset+seq.weight_offset)
            weights=dict(zip((b.name for b in bones),buf.read_fmt(str(len(bones))+'f')));buf.seek(saved)
            for index in seq.anim_desc_indices:
                self.sequences[animations[index].name]=(seq,weights)
        self.attachment_model=self.resource('mm/models/npc/necroguard/npc_necroguard.mdl')
        buf,header,attachment_bones,_=load_mdl(self.attachment_model,ROOT/'third_party/SourceIO')
        from SourceIO.library.models.mdl.structs.attachment import Attachment
        buf.seek(header.local_attachment_offset);self.attachments={}
        for _ in range(header.local_attachment_count):
            at=Attachment.from_buffer(buf,header.version);m=np.array(at.matrix).reshape(3,4)
            self.attachments[at.name]=(attachment_bones[at.parent_bone].name,to4(m[:,:3],m[:,3]))
        self.sword_idle=self.item_idle('mm/models/items/weapons/sword_long/sword_long.mdl')
        self.shield_idle=self.item_idle('mm/models/items/armors/shield_guard/shield_guard.mdl')
        attachment=self.attachments['RightHand'][1][:3,:3]@self.sword_idle[:3,:3]
        native=self.nodes['Weapon Bone'].rotation
        self.hand_map={'R':frame(attachment@[0,-1,0],attachment@[0,0,-1])@frame(native@[0,1,0],native@[1,0,0]).T}
        locals_={b.name:np.asarray(b.position) for b in attachment_bones}
        self.hand_map['L']=frame(locals_['SK_L_Finger_B1'],locals_['SK_L_Thumb_1'])@frame(self.nodes['Bip01 L Finger2'].translation,self.nodes['Bip01 L Finger0'].translation).T
        # Morrowind shield meshes: face normal +Z, top edge +Y. The source
        # shield's face normal is its item -Y and its top edge is item +Z.
        self.shield_map=frame(self.shield_idle[:3,:3]@[0,-1,0],self.shield_idle[:3,:3]@[0,0,1])@frame(np.array([0,0,1.]),np.array([0,1.,0])).T
        self.arm_scale={};self.dm_hinge={};self.hinge={};self.roll_state={};self.segment_records=[];self.group_records={}
        idle=self.source_world(self.clip('guard','@combatidle'),0)
        for side,names in SIDES.items():
            _,shoulder,elbow,hand=names['dm'];_,upper,fore,target_hand=names['mw']
            s,e,h=(idle[n][:3,3] for n in (shoulder,elbow,hand))
            self.arm_scale[side]=float((norm(self.nodes[fore].translation)+norm(self.nodes[target_hand].translation))/(norm(e-s)+norm(h-e)))
            normal=cross(e-s,h-e);normal/=norm(normal)
            self.dm_hinge[side]=idle[elbow][:3,:3].T@normal
            R=self.nodes[fore].rotation
            axis=np.array([R[2,1]-R[1,2],R[0,2]-R[2,0],R[1,0]-R[0,1]])
            self.hinge[side]=axis/norm(axis)
        self.mapping=dict(self.mapping,**{'Weapon Bone':'__native_weapon','Shield Bone':'__source_shield'})

    def resource(self,name):
        path=self.work/Path(name).name
        if not path.exists():
            for archive in reversed(archives(self.vpk)):
                found=next((e for n,e in entries(archive) if n.lower()==name.lower()),None)
                if found is not None:store(path,extract(archive,found));break
            else:raise FileNotFoundError(name)
        return path

    def item_idle(self,name):
        buf,_,bones,animations=load_mdl(self.resource(name),ROOT/'third_party/SourceIO')
        anim=next(a for a in animations if a.name=='@idle');tracks=anim.read_animations(buf,bones)
        root=bones[0];track=tracks[root.name]
        return to4(quat_matrix(track['rot'][0]),track['pos'][0])@np.linalg.inv(to4(quat_matrix(root.quat),root.position))

    def source_world(self,clip,t):
        bones,tracks,anim=clip;result={};at=min(max(0,t*anim.fps),anim.frame_count-1)
        weights=self.sequences.get(anim.name,(None,{}))[1]
        fallback=None
        if any(v<.999 for v in weights.values()):
            fallback=self.source_world(self.clip('guard','@combatidle'),t%self.duration(self.clip('guard','@combatidle')))
        for b in bones:
            parent=result[bones[b.parent_id].name] if b.parent_id>=0 else np.eye(4)
            weight=weights.get(b.name,1)
            if weight==0:
                local=np.linalg.inv(fallback[bones[b.parent_id].name])@fallback[b.name] if b.parent_id>=0 else fallback[b.name]
            else:
                track=tracks.get(b.name)
                q=sample_quaternion(track['rot'],at) if track is not None else b.quat
                p=np.array([np.interp(at,np.arange(len(track['pos'])),track['pos'][:,j]) for j in range(3)]) if track is not None else b.position
                local=to4(quat_matrix(q),p)
                if weight<1:
                    baseline=np.linalg.inv(fallback[bones[b.parent_id].name])@fallback[b.name] if b.parent_id>=0 else fallback[b.name]
                    mixed=sample_quaternion(np.array([matrix_quat(baseline[:3,:3]),matrix_quat(local[:3,:3])]),weight)
                    local=to4(quat_matrix(mixed[[1,2,3,0]]),baseline[:3,3]*(1-weight)+local[:3,3]*weight)
            result[b.name]=parent@local
        return result

    def target_world(self,pose):
        result={}
        def visit(name):
            if name not in result:
                node=self.nodes[name];q=pose.get(name)
                R=quat_matrix(q[[1,2,3,0]]) if q is not None else node.rotation
                p=pose.get('_translations',{}).get(name,node.translation)
                result[name]=(visit(self.parents[name]) if name in self.parents else np.eye(4))@to4(R,p)
            return result[name]
        for name in self.nodes:visit(name)
        return result

    def pose(self,base,time,arms=None,arms_time=None):
        pose=super().pose(base,time,arms,arms_time)
        if not hasattr(self,'hand_map'):return pose
        dm=self.source_world(base,time)
        # Respect Source's zero-weight lower-body mask before arm retargeting.
        weights=self.sequences.get(base[2].name,(None,{}))[1]
        if any(v<.999 for v in weights.values()):
            fallback=super().pose(self.clip('guard','@combatidle'),time%self.duration(self.clip('guard','@combatidle')))
            for target,source in self.mapping.items():
                if weights.get(source,1)==0 and target in fallback:pose[target]=fallback[target]
        world=self.target_world(pose)
        for side,names in SIDES.items():
            clav,upper,fore,hand=names['mw'];_,ds,de,dh=names['dm'];scale=self.arm_scale[side]
            shoulder=world[upper][:3,3];Rh=self.basis@dm[dh][:3,:3]@self.hand_map[side]
            target=shoulder+scale*self.basis@(dm[dh][:3,3]-dm[ds][:3,3])
            if side=='R':
                handle=dm[self.attachments['RightHand'][0]]@self.attachments['RightHand'][1]@self.sword_idle
                target=shoulder+scale*self.basis@(handle[:3,3]-dm[ds][:3,3])-Rh@self.nodes['Weapon Bone'].translation
            L1=norm(self.nodes[fore].translation);L2=norm(self.nodes[hand].translation)
            reach=target-shoulder;distance=norm(reach);axis=reach/distance
            d=np.clip(distance,abs(L1-L2)+1e-3,L1+L2-1e-3);target=shoulder+axis*d
            normal=self.basis@dm[de][:3,:3]@self.dm_hinge[side];bend=cross(axis,normal)
            if norm(bend)<1e-6:bend=cross(axis,[0,0,1])
            bend/=norm(bend);a=(L1*L1-L2*L2+d*d)/(2*d)
            elbow=shoulder+axis*a+bend*np.sqrt(max(L1*L1-a*a,0))
            normal=cross(elbow-shoulder,target-elbow);normal/=norm(normal)
            hinge=self.hinge[side];perp=hinge-np.array([1,0,0])*hinge[0]
            reference=frame(np.array([1.,0,0]),perp)
            Ru=frame(elbow-shoulder,normal)@reference.T;Rf=frame(target-elbow,normal)@reference.T
            local=Rf.T@Rh;twist=np.arctan2(local[2,1]-local[1,2],local[1,1]+local[2,2])
            if side in self.roll_state:
                prev=self.roll_state[side];twist=prev+np.arctan2(np.sin(twist-prev),np.cos(twist-prev))
            self.roll_state[side]=twist;c,s=np.cos(twist/2),np.sin(twist/2)
            Rf=Rf@np.array([[1,0,0],[0,c,-s],[0,s,c]])
            pose[upper]=matrix_quat(world[clav][:3,:3].T@Ru)
            pose[fore]=matrix_quat(Ru.T@Rf);pose[hand]=matrix_quat(Rf.T@Rh)
        pose['Weapon Bone']=matrix_quat(self.nodes['Weapon Bone'].rotation)
        world=self.target_world(pose)
        parent,attachment=self.attachments['LeftShield'];shield=dm[parent]@attachment
        Rs=self.basis@shield[:3,:3]@self.shield_map
        pose['Shield Bone']=matrix_quat(world['Bip01 L Hand'][:3,:3].T@Rs)
        # NPC shields attach to the Source forearm, not the viewmodel wrist.
        # Keep that moving mount relative to the solved native elbow; copying
        # the native hand mount would move the shield whenever the wrist bends.
        origin=world['Bip01 L Forearm'][:3,3]+self.arm_scale['L']*self.basis@(shield[:3,3]-dm['SK_L_Elbow'][:3,3])
        pose['_translations']={'Shield Bone':world['Bip01 L Hand'][:3,:3].T@(origin-world['Bip01 L Hand'][:3,3])}
        return pose

    def globals(self, clip, t):
        if hasattr(self,'sequences'):
            return {name:matrix[:3,:3] for name,matrix in self.source_world(clip,t).items()}
        bones,tracks,anim=clip; cache={}; frame=min(max(0,t*anim.fps),anim.frame_count-1)
        def get(i):
            b=bones[i]
            if i not in cache:
                q=sample_quaternion(tracks[b.name]['rot'],frame) if b.name in tracks else b.quat
                cache[i]=(get(b.parent_id) if b.parent_id>=0 else np.eye(3))@quat_matrix(q)
            return cache[i]
        return {b.name:get(n) for n,b in enumerate(bones)}

    @staticmethod
    def root_height(clip,t):
        bones,tracks,anim=clip
        if 'SK_Root' not in tracks:
            return float(next(b.position[2] for b in bones if b.name=='SK_Root'))
        values=tracks['SK_Root']['pos']
        return float(np.interp(t*anim.fps,np.arange(len(values)),values[:,2]))

    def segment(self, base, arms=None, duration=None, loop=False):
        if arms: raise ValueError('NPCs use complete guard clips, not viewmodel overlays')
        duration = duration or self.duration(base)
        times=np.linspace(0,duration,max(2,round(duration*FPS)+1))
        poses=[];self.roll_state={}
        for t in times:
            source_time=float(t)*self.duration(base)/duration
            pose=self.pose(base,source_time)
            # Retain vertical body movement. Native accumulated Bip01 tracks own
            # horizontal travel, preventing a second movement through geometry.
            root_height=float(self.source_world(base,source_time)['SK_Root'][2,3])
            dz=(root_height-self.reference_height)*self.height_scale
            pose['_pelvis_translation']=self.nodes['Bip01 Pelvis'].translation + self.rest[self.parents['Bip01 Pelvis']].T@np.array([0,0,dz])
            poses.append(pose)
        if loop:
            # Loop clips should join continuously despite compressed endpoint drift.
            poses[-1]={name:value.copy() for name,value in poses[0].items()}
        self.segment_records.append(dict(clip=base[2].name,duration=float(duration),natural=self.duration(base),loop=loop))
        return times,poses

    def write(self,out,group,segments,textkeys,root_velocity=None):
        super().write(out,group,segments,textkeys,root_velocity)
        nif=self.nif;path=out/(group+'.kf');stream=nif.NiStream();stream.load(path)
        names=[e.string_data for e in stream.root.extra_datas if isinstance(e,nif.NiStringExtraData)]
        controller=dict(zip(names,stream.root.controllers))['Bip01 Pelvis']
        rows=[[float(offset+t),*pose['_pelvis_translation']] for offset,times,poses in segments for t,pose in zip(times,poses)]
        controller.data.translations.keys=np.array(rows,np.float32)
        shield_controller=dict(zip(names,stream.root.controllers))['Shield Bone']
        shield_controller.data.translations.keys=np.array([[float(offset+t),*pose['_translations']['Shield Bone']] for offset,times,poses in segments for t,pose in zip(times,poses)],np.float32)
        # One arm controller owns the entire arm in OpenMW: absent fingers keep
        # old transforms rather than falling through to the armed idle source.
        for name,q in self.grip.items():
            if name in names:
                ctrl=list(stream.root.controllers)[names.index(name)]
            else:
                ctrl=nif.NiKeyframeController(data=nif.NiKeyframeData(),start_time=0.,stop_time=float(max(row[0] for row in rows)),flags=12)
                stream.root.extra_datas.append(nif.NiStringExtraData(string_data=name));stream.root.controllers.append(ctrl)
            armed=group.endswith('1h') or group in ('weapononehand','brutal_hit','shield') or group.startswith(('hit','brutal_clinch'))
            q=q if armed else matrix_quat(self.nodes[name].rotation)
            ctrl.data.rotations=nif.NiRotData(keys=np.array([[rows[0][0],*q],[rows[-1][0],*q]],np.float32))
            ctrl.data.translations=nif.NiPosData(keys=np.array([[0,*self.nodes[name].translation]],np.float32))
        stream.save(path)
        yaml="blending_rules:\n  - from: '*'\n    to: '*'\n    easing: linear\n    duration: 0.2\n"
        for attack in ('chop','slash','thrust'):
            for source,target in [(attack+' start',attack+' max attack')]+[(attack+' max attack',attack+' '+strength+' follow start') for strength in ('small','medium','large')]:
                yaml+=f"  - from: 'weapononehand:{source}'\n    to: 'weapononehand:{target}'\n    easing: linear\n    duration: 0\n"
        yaml+="  - from: ''\n    to: '*'\n    easing: linear\n    duration: 0\n"
        path.with_suffix('.yaml').write_text(yaml)
        check=nif.NiStream();check.load(path)
        assert all(np.isfinite(c.data.rotations.keys).all() for c in check.root.controllers)
        self.group_records[group]={'segments':self.segment_records,'markers':[(float(t),label) for t,label in textkeys]}
        self.segment_records=[]

    def loop_guard(self,out,group,clip,velocity=None):
        t,p=self.segment(self.clip('guard',clip),loop=True)
        self.write(out,group,[(0,t,p)],[(0,'start'),(0,'loop start'),(t[-1],'loop stop'),(t[-1],'stop')],velocity)

    def clinch(self, out):
        # SCHED_MM_WEAPON_VS_WEAPON_ACTIVE/WIN/LOOSE select these activities.
        # Keep their authored durations; the native paired contest interrupts
        # the idle when the player's shorter 44-frame animation has finished.
        for group, clip in [('brutal_clinch', '@versus_right_idle'),
                            ('brutal_clinch_win', '@versus_right_out_win'),
                            ('brutal_clinch_lose', '@versus_left_out_lost')]:
            t, p = self.segment(self.clip('guard', clip))
            marks = [(0, 'start'), (float(t[-1]), 'stop')]
            self.write(out, group, [(0, t, p)], marks)

    def weapon_guard(self,out):
        segments=[];marks=[(0,'start'),(0,'brutal continuous release')];offset=0.
        for phase,clip in [('equip','@unsheathe_sword'),('unequip','@sheathe_sword')]:
            t,p=self.segment(self.clip('guard',clip));d=float(t[-1])
            segments.append((offset,t,p))
            marks += [(offset,phase+' start'),(offset+d*.4,phase+(' attach' if phase=='equip' else ' detach')),(offset+d,phase+' stop')]
            offset+=d+.05
        for attack,clip in [('chop','@attack_top'),('slash','@attack_left'),('thrust','@attack_bottom')]:
            t,p=self.segment(self.clip('guard',clip));d=float(t[-1])
            events={e.event:float(e.cycle)*d for e in self.sequences[clip][0].events}
            if 2152 not in events or 2102 not in events:raise ValueError('Missing source swing/hit events: '+clip)
            wind=events[2152];hit=events[2102]
            segments.append((offset,t,p))
            marks += [(offset,attack+' start'),(offset+.15,attack+' min attack'),(offset+wind,attack+' max attack'),
                      (offset+wind,attack+' min hit'),(offset+hit,attack+' hit')]
            for strength in ('small','medium','large'):
                marks += [(offset+hit+.001,attack+' '+strength+' follow start'),(offset+d,attack+' '+strength+' follow stop')]
            offset+=d+.05
        marks.append((offset-.05,'stop'))
        self.write(out,'weapononehand',segments,marks)


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root',type=Path,required=True)
    ap.add_argument('--work-dir',type=Path,required=True)
    ap.add_argument('--skeleton',type=Path,required=True)
    ap.add_argument('--grip-animation',type=Path,required=True)
    ap.add_argument('--output',type=Path,required=True)
    ap.add_argument('--clinch-only',action='store_true',help='Emit only the three additive sword-clinch clips')
    a=ap.parse_args();a.work_dir.mkdir(parents=True,exist_ok=True)
    out=a.output/'Animations/brutal_npc';out.mkdir(parents=True,exist_ok=True)
    obsolete=[p.name for p in out.glob('*.kf') if p.stem.startswith(('sneak','jump')) or p.stem=='idlesneak']
    if obsolete:
        raise ValueError('Player-derived NPC clips remain: '+', '.join(sorted(obsolete))
                         +'. Generate into a clean data directory and replace the previous NPC directory with it.')
    i=GuardImporter(a.work_dir,a.vpk_root,a.skeleton,a.grip_animation)
    if a.clinch_only:
        i.clinch(out)
        sources=[a.skeleton,a.grip_animation,i.attachment_model,a.work_dir/'npc_guard_combat_animations.mdl']
        (a.output/'npc-clinch-import.json').write_text(json.dumps(dict(
            converter='import_npc.py --clinch-only',derived_from_game_files=True,clips=i.provenance,groups=i.group_records,
            inputs={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in sources},
            outputs={path.relative_to(a.output).as_posix():hashlib.sha256(path.read_bytes()).hexdigest()
                     for path in sorted(out.glob('brutal_clinch.*'))+sorted(out.glob('brutal_clinch_*.*'))},
            notes='Original guard ACT_VERSUS_RIGHT_IDLE/RIGHT_OUT_WIN/LEFT_OUT_LOOSE, natural durations; native collision owns horizontal displacement; sword finger grip retained.'
        ),indent=2)+'\n')
        return
    i.loop_guard(out,'idle','@idle_0');i.loop_guard(out,'idle1h','@combatidle')
    for mode,src,speed in [('walk','Walk',150),('run','Run',330)]:
        for direction,compass in [('forward','N'),('back','S'),('left','W'),('right','E')]:
            vector={'forward':(0,speed,0),'back':(0,-speed,0),'left':(-speed,0,0),'right':(speed,0,0)}[direction]
            for suffix in ('','1h'):
                clip='a_'+('Combatwalk' if mode=='walk' and suffix else src)+compass
                i.loop_guard(out,mode+direction+suffix,clip,vector)
    i.weapon_guard(out)
    i.clinch(out)
    t,p=i.segment(i.clip('guard','@Block'))
    i.write(out,'shield',[(0,t,p)],[(0,'start'),(0,'block start'),(float(t[-1]),'block stop'),(float(t[-1]),'stop')])
    for group,clip,duration in [('brutal_hit','@Hit_Front_medium',5/6),('hit1','anm_Hit_Body_Front_small',None),
                                ('hit2','anm_Hit_Body_left_small',None),('hit3','anm_Hit_Body_right_small',None),
                                ('hit4','anm_Hit_Body_back_small',None),('hit5','anm_Hit_head_Front_small',None)]:
        t,p=i.segment(i.clip('guard',clip),duration=duration)
        markers=[(0,'start'),(float(t[-1]),'stop')]
        if group.startswith('hit'):markers.append((0,'brutal upper body'))
        i.write(out,group,[(0,t,p)],markers)
    # Dark Messiah's kick reactions (ACT_PUSH_*): the engine picks the side the kick came
    # from, head or body, and strength. Native physics supplies the displacement.
    for side in ('Front','Back','Left','Right'):
        for part in ('Head','Body'):
            group='brutal_push_'+part.lower()+'_'+side.lower()
            # The source spells one of them '@Push_Body_back_small'.
            name='@Push_'+part+'_'+(side.lower() if (part,side)==('Body','Back') else side)+'_small'
            t,p=i.segment(i.clip('guard',name))
            i.write(out,group,[(0,t,p)],[(0,'start'),(float(t[-1]),'stop')])
        for strength in ('medium','long'):
            group='brutal_push_'+side.lower()+'_'+strength
            t,p=i.segment(i.clip('guard','@Push_'+(side if side=='Front' else side.lower())+'_'+strength))
            i.write(out,group,[(0,t,p)],[(0,'start'),(float(t[-1]),'stop')])
    # SCHED_MM_STAND_UP_FRONT/BACK after a living ragdoll fall: front lies face down, back
    # face up; both start with the head behind the actor's final facing. ACT_MELEE_PARRY_BREAK
    # (@Parry_Broken) is the attacker's recoil after a timed player parry.
    for group,clip in [('brutal_standup_front','@StandUp_front'),('brutal_standup_back','@StandUp_back'),
                       ('brutal_parrybroken','@Parry_Broken')]:
        t,p=i.segment(i.clip('guard',clip))
        i.write(out,group,[(0,t,p)],[(0,'start'),(float(t[-1]),'stop')])
    (a.output/'npc-import.json').write_text(json.dumps({'converter':'import_npc.py','clips':i.provenance,
        'source_sha256':hashlib.sha256((a.work_dir/'npc_guard_combat_animations.mdl').read_bytes()).hexdigest(),
        'outputs':{path.name:hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(out.iterdir()) if path.suffix in ('.kf','.yaml')},
        'inputs':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in (a.skeleton,a.grip_animation,i.attachment_model)},
        'coverage':{'converted':['idle','one-handed sword','shield block','small upper-body hit reactions','heavy full-body kick reaction','walk/run in four directions'],
                    'fallback':['two-handed melee','hand-to-hand','bow','crossbow','thrown weapons','spell casting','sneak','jump','death/get-up']},
        'groups':i.group_records,'arm_scales':i.arm_scale,
        'attachments':{name:{'parent':parent,'matrix':matrix.tolist()} for name,(parent,matrix) in i.attachments.items() if name in ('RightHand','LeftShield')},
        'hand_maps':{side:m.tolist() for side,m in i.hand_map.items()},
        'finger_calibration':{'right':'native idle1h: start','left':'native shield: block hit','channels':sorted(i.grip)},
        'notes':'Actual guard clips and necroguard RightHand/LeftShield attachments; native hand-to-hilt and finger grip; source upper-body masks; native horizontal root travel. Attack swing/hit keys use source events2152/2102.'},indent=2)+'\n')


if __name__=='__main__':main()
