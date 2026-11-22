#!/usr/bin/env python3
"""Import Dark Messiah's body-awareness clips for the first-person body.

The clips use the model's @reference pose and keep local pelvis motion, scaled by
target/source leg length. Only body and leg tracks are emitted; the arms come from
the arm importers.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from retarget_kf import cross, norm

from import_first_person import Importer, ROOT, FPS
from retarget_kf import matrix_quat


class BodyAwareness(Importer):
    def __init__(self, work, vpk, skeleton, armed_idle='@idle_leftfoot', root_motion='pelvis'):
        self.armed_idle = armed_idle
        self.root_motion = root_motion
        super().__init__(work, vpk, skeleton, arm_pitch=0)
        self.grip = {}
        self.mapping = {name: source for name, source in self.mapping.items()
                        if not any(part in name for part in
                                   ('Head', 'Neck', 'Clavicle', 'UpperArm', 'Forearm', 'Hand'))}
        reference = self.clip('player_animations', '@reference')
        self.reference = self.globals(reference, 0.)
        source_bones = {bone.name: bone for bone in reference[0]}
        ratios = []
        for side in 'LR':
            target = sum(norm(self.nodes[f'Bip01 {side} {joint}'].translation)
                         for joint in ('Calf', 'Foot'))
            source = sum(norm(source_bones[f'SK_{side}_{joint}'].position)
                         for joint in ('Knee', 'Ankle'))
            ratios.append(float(target/source))
        self.leg_scale = float(np.mean(ratios))
        self.reference_root = self.root_position(reference, 0.)

    def camera_contract(self):
        """Use the pelvis as the common anchor for differently proportioned rigs."""
        world = {}
        def get(name):
            if name not in world:
                node = self.nodes[name]
                matrix = np.eye(4)
                matrix[:3, :3] = node.rotation
                matrix[:3, 3] = node.translation
                world[name] = (get(self.parents[name]) if name in self.parents else np.eye(4))@matrix
            return world[name]
        native_pelvis = get('Bip01 Pelvis')[:3, 3]
        source_eye = np.array([0., 0., 61.065])
        native_eye = native_pelvis+self.leg_scale*self.basis@(source_eye-self.reference_root)
        return {
            'schema': 1,
            'body_scale': self.leg_scale,
            'source_base_eye': source_eye.tolist(),
            'source_pelvis_reference': self.reference_root.tolist(),
            'native_pelvis_reference': native_pelvis.tolist(),
            'native_base_eye': native_eye.tolist(),
            'pivot_node': 'Brutal Camera Pivot',
            'notes': 'Actual DM view offset is the base eye. CameraAdd is added absolutely, '
                     'rotated by the view, to the physical camera; it is not a zeroed delta.'
        }

    def clip(self, model, name):
        # Dark Messiah's local-player body uses merged sequence 7: player_basic's five
        # local sequences followed by player_animations sequence 2 (idle_leftfoot).
        # Combat_Idle is a distinct forward-leaning pose and belongs to other uses.
        if model == 'player_animations' and name == '@Combat_Idle':
            name = self.armed_idle
        return super().clip(model, name)

    @staticmethod
    def root_position(clip, time):
        bones, tracks, animation = clip
        root = next(bone for bone in bones if bone.name == 'SK_Root')
        if root.parent_id >= 0:
            raise ValueError('Expected SK_Root to be the body animation root')
        track = tracks.get(root.name)
        if track is None:
            return np.array(root.position, dtype=float)
        frame = min(int(round(time*animation.fps)), animation.frame_count-1)
        return np.array(track['pos'][min(frame, len(track)-1)], dtype=float)

    def pose(self, base, time, arms=None, arms_time=None):
        result = super().pose(base, time, arms, arms_time)
        delta = self.basis@(self.root_position(base, time)-self.reference_root)*self.leg_scale
        result['_root_delta'] = self.rest['Bip01'].T@delta
        return result

    @staticmethod
    def blend_pose(a, b, weight):
        out = {}
        for name in a.keys() | b.keys():
            left, right = a.get(name, b.get(name)), b.get(name, a.get(name))
            if name == '_root_delta':
                out[name] = (1-weight)*left+weight*right
            else:
                if np.dot(left, right) < 0:
                    right = -right
                q = (1-weight)*left+weight*right
                out[name] = q/norm(q)
        return out

    def sword_kick(self, out, height='medium'):
        # Same source timeline and sequence fades as the primary rigid rig. The
        # shared completion clock must include the return tail on both instances.
        kick = self.clip('player_animations', '@attack_kick_'+height)
        idle = self.clip('player_animations', self.armed_idle)
        duration, fade = self.duration(kick), 0.20000000298023224
        end = duration+fade
        times = np.linspace(0., end, round(end*FPS)+1)
        rest = self.pose(idle, 0.)
        final = self.pose(kick, duration)
        poses = []
        for t in times:
            if t <= duration:
                pose = self.pose(kick, float(t))
                if t < fade:
                    pose = self.blend_pose(rest, pose, float(t/fade))
            else:
                pose = self.blend_pose(final, rest, float((t-duration)/fade))
            poses.append(pose)
        self.write(out, 'brutal_kick1h' if height == 'medium' else 'brutal_kick1h_'+height, [(0., times, poses)],
                   [(0., 'start'), (.375, 'hit'), (end, 'stop')])

    def faded(self, clip, duration=None, fade_in=True, fade_out=True, loop=False):
        # Source sequence fades (0.2 s) from and back to the armed idle, as for the kick.
        idle = self.clip('player_animations', self.armed_idle)
        rest, fade = self.pose(idle, 0.), 0.20000000298023224
        natural = self.duration(clip)
        duration = duration or natural
        tail = fade if fade_out else 0.
        times = np.linspace(0., duration+tail, max(2, round((duration+tail)*FPS)+1))
        final = self.pose(clip, natural)
        poses = []
        for t in times:
            if t <= duration:
                source = float(t)*natural/duration if loop else min(float(t), natural)
                pose = self.pose(clip, source)
                if fade_in and t < fade:
                    pose = self.blend_pose(rest, pose, float(t/fade))
            else:
                pose = self.blend_pose(final, rest, float((t-duration)/fade))
            poses.append(pose)
        if loop:
            poses[-1] = dict(poses[0])
        return times, poses

    def combos(self, out):
        # Rush attack: the full-body lunge of layer_combo_sword_rush_noshield.
        times, poses = self.faded(self.clip('player_animations', 'anm_combo_sword_rush_noshield'))
        self.write(out, 'brutal_rush', [(0., times, poses)], [(0., 'start'), (times[-1], 'stop')])
        # Falling attack: airborne body while the sword is raised and held, then the
        # landing strike. Segment keys match the arms so the shared clock maps phases.
        air = self.clip('player_animations', '@jump_idle')
        strike = self.clip('player_animations', 'anm_combo_sword_fall_noshield')
        t1, p1 = self.faded(air, .3, fade_out=False)
        t2, p2 = self.faded(air, self.duration(air), fade_in=False, fade_out=False, loop=True)
        t3, p3 = self.faded(strike, fade_in=False)
        hold = t1[-1]+.001; out_at = hold+t2[-1]+.001; end = out_at+t3[-1]
        self.write(out, 'brutal_fallattack', [(0., t1, p1), (hold, t2, p2), (out_at, t3, p3)],
                   [(0., 'start'), (hold, 'loop start'), (hold+t2[-1], 'loop stop'), (out_at, 'out start'), (end, 'stop')])

    def write(self, out, group, segments, textkeys, root_velocity=None):
        # Existing segment construction supplies identical marker timing. SK_Root
        # is the pelvis, not native actor accumulation. Its local shifts keep the
        # support foot planted during the kick without moving the collision body.
        super().write(out, group, segments, textkeys)
        nif = self.nif
        path = out/(group+'.kf')
        stream = nif.NiStream(); stream.load(path)
        samples = [(float(offset+time), pose['_root_delta'])
                   for offset, times, poses in segments for time, pose in zip(times, poses)]
        duration = samples[-1][0]
        values = np.array([[0., *self.nodes['Bip01'].translation],
                           [duration, *self.nodes['Bip01'].translation]], dtype=np.float32)
        if self.root_motion == 'vertical':
            values = np.array([[time, *self.nodes['Bip01'].translation] for time, delta in samples], np.float32)
            values[:, 3] += np.array([delta[2] for time, delta in samples], np.float32)
        else:
            names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
            pelvis = list(stream.root.controllers)[names.index('Bip01 Pelvis')]
            positions = np.array([[time, *(self.nodes['Bip01 Pelvis'].translation+delta)]
                                  for time, delta in samples], dtype=np.float32)
            pelvis.data.translations = nif.NiPosData(keys=positions)
        duration = float(values[-1, 0])
        q = matrix_quat(self.nodes['Bip01'].rotation)
        data = nif.NiKeyframeData(
            rotations=nif.NiRotData(keys=np.array([[0., *q], [duration, *q]], np.float32)),
            translations=nif.NiPosData(keys=values))
        stream.root.extra_datas.append(nif.NiStringExtraData(string_data='Bip01'))
        stream.root.controllers.append(nif.NiKeyframeController(
            data=data, start_time=0., stop_time=duration, flags=12))
        stream.save(path)
        check = nif.NiStream(); check.load(path)
        names = [e.string_data for e in check.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        assert names.count('Bip01') == 1
        assert not any(any(part in name for part in
                           ('Head', 'Neck', 'Clavicle', 'UpperArm', 'Forearm', 'Hand', 'Finger')) for name in names)
        np.testing.assert_allclose(list(check.root.controllers)[names.index('Bip01')].data.translations.keys,
                                   values, atol=1e-6)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--vpk-root', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--skeleton', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--armed-idle', default='@idle_leftfoot',
                   help='Local-player body idle; @Combat_Idle gives the forward-leaning combat stance')
    p.add_argument('--root-motion', choices=['pelvis', 'vertical'], default='pelvis',
                   help='Source local pelvis XYZ; vertical keeps only the height')
    a = p.parse_args()
    a.work_dir.mkdir(parents=True, exist_ok=True)
    out = a.output/'Animations/brutal_firstperson'
    out.mkdir(parents=True, exist_ok=True)
    imp = BodyAwareness(a.work_dir, a.vpk_root, a.skeleton, a.armed_idle, a.root_motion)
    # The contract depends on the two rigs only. Arm importers wait for this file,
    # not for the clips below, so it appears first and in one step.
    contract = json.dumps(imp.camera_contract(), indent=2)+'\n'
    partial = a.output/'body-camera-contract.json.part'
    partial.write_text(contract)
    partial.replace(a.output/'body-camera-contract.json')
    imp.loop(out, 'idle', '@idle_middle')
    imp.loop(out, 'idle1h', '@Combat_Idle', 'anm_sword_noshield_idle')
    imp.loop(out, 'idlesneak', '@Crouch_Idle')
    for mode, source in [('walk', 'Walk'), ('run', 'Run'), ('sneak', 'Crouch')]:
        for direction, compass in [('forward', 'N'), ('back', 'S'), ('left', 'W'), ('right', 'E')]:
            clip = 'a_'+source+compass
            imp.loop(out, mode+direction, clip)
            imp.loop(out, mode+direction+'1h', clip, '@run_sword' if mode == 'run' else '@walk_sword')
    # Dark Messiah's Shift sprint; the engine plays it in place of the forward run.
    imp.loop(out, 'brutal_sprint', 'a_sprintN')
    imp.loop(out, 'brutal_sprint1h', 'a_sprintN', '@Megasprint_sword')
    imp.weapon(out)
    imp.sword_kick(out)
    # Dark Messiah kicks low when the view is pitched down at least 50 degrees.
    imp.sword_kick(out, 'low')
    imp.shield_block(out, 0)
    imp.combos(out)
    imp.jump(out, 'jump')
    imp.jump(out, 'jump1h', True)
    (a.output/'body-awareness-import.json').write_text(json.dumps({
        'converter': str(Path(__file__).relative_to(ROOT)),
        'reference': 'player_animations/@reference',
        'armed_idle': a.armed_idle,
        'leg_scale': imp.leg_scale,
        'reference_root': imp.reference_root.tolist(),
        'root_motion': a.root_motion,
        'kick_fade_in_seconds': 0.20000000298023224,
        'kick_return_tail_seconds': 0.20000000298023224,
        'clips': imp.provenance,
        'notes': 'Derived from the user\'s own game files. Body/leg rotations and source local pelvis motion only. '
                 'Native actor physics supplies world motion. Hit markers remain integration markers.'
    }, indent=2)+'\n')
    if json.dumps(imp.camera_contract(), indent=2)+'\n' != contract:
        raise SystemExit('Body-camera contract changed while the clips were generated')


if __name__ == '__main__':
    main()
