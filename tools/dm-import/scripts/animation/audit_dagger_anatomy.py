#!/usr/bin/env python3
"""Compare the fast-D wrist pose against the raw Source bones."""
import argparse
import json
from pathlib import Path
import numpy as np
from retarget_kf import norm
from audit_viewmodel_continuity import tracks
from audit_dagger_combat_clock import quaternion_at
from audit_weapon_family_fingers import angle
from import_dagger_anatomy import DaggerAnatomy, FRAME_NODES
from import_viewmodel import Viewmodel, to4


def sample(channels, time):
    return {name: (quaternion_at(rotations, time),
            np.array([np.interp(time, positions[:, 0], positions[:, i]) for i in (1, 2, 3)]))
            for name, (rotations, positions) in channels.items()}


def source_local(vm, model, clip, time):
    world = Viewmodel.dm_world(vm, model, clip, time)
    result = {}
    for bone in vm.dm_bones(model):
        local = (np.linalg.inv(world[vm.dm_bones(model)[bone.parent_id].name]) @ world[bone.name]
                 if bone.parent_id >= 0 else world[bone.name])
        result[bone.name] = local[:3, :3], local[:3, 3]
    return result


def source_fk(vm, model, pose):
    result = {}
    bones = vm.dm_bones(model)
    for bone in bones:
        local = to4(*pose[bone.name])
        result[bone.name] = result[bones[bone.parent_id].name] @ local if bone.parent_id >= 0 else local
    return {name.casefold(): value for name, value in result.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--clip', choices=('B', 'C', 'D', 'E'), default='D')
    parser.add_argument('--locals', type=float, nargs='+', default=[0., .0666667, .116665142877, .133331554877, .1666667, .2121212])
    for name in ('data', 'baseline', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    vm = DaggerAnatomy(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                       camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    old = tracks(args.baseline / 'Animations/brutal_fp_dagger/weapononehand.kf')
    new = tracks(args.data / 'Animations/brutal_fp_dagger/weapononehand.kf')
    info = json.loads((args.data / 'weapon-families-import.json').read_text())['dagger']['groups']['brutal_fp_dagger']['weapononehand']
    predecessor = chr(ord(args.clip) - 1)
    c = next(segment for segment in info['segments'] if segment['clip'] == 'anm_fastattack_daggers_' + predecessor)
    d = next(segment for segment in info['segments'] if segment['clip'] == 'anm_fastattack_daggers_' + args.clip)
    old_c = sample(old, c['offset'] + c['duration'])
    new_c = sample(new, c['offset'] + c['duration'])
    source_c = source_local(vm, c['model'], c['clip'], c['source_natural'])
    report = {'method': 'Preceding clip end to current clip local-transform slerp, native 0.2s arm fade', 'clip': args.clip, 'rows': []}
    for local in args.locals:
        alpha = min(1., local / .2)
        worlds = [vm.fk(Viewmodel.blend_pose(start, sample(channels, d['offset'] + local), alpha))
                  for start, channels in ((old_c, old), (new_c, new))]
        source_d = source_local(vm, d['model'], d['clip'], local * d['source_rate'])
        source = source_fk(vm, d['model'], Viewmodel.blend_pose(source_c, source_d, alpha))
        row = dict(local=local, alpha=alpha, sides={})
        for side in 'LR':
            names = [f'sk_{side.lower()}_finger_{letter}1' for letter in 'abcd']
            if side == 'R': names[-1] = 'sk_finger_d1'
            wrist = source[f'sk_{side.lower()}_wrist'][:3, 3]
            elbow = source[f'sk_{side.lower()}_elbow'][:3, 3]
            palm = np.mean([source[name][:3, 3] for name in names], axis=0)
            values = dict(source_bend_degrees=angle(wrist - elbow, palm - wrist))
            mean = np.mean([vm.local(f'Bip01 {side} Finger{i}')[1] for i in (1, 2, 3, 4)], axis=0)
            for label, world in zip(('before', 'after'), worlds):
                hand, forearm = world[f'Bip01 {side} Hand'], world[f'Bip01 {side} Forearm']
                values[label + '_bend_degrees'] = angle(hand[:3, 3] - forearm[:3, 3], hand[:3, :3] @ mean)
            old_h, new_h = (world[f'Bip01 {side} Hand'] for world in worlds)
            values['hand_position_delta_units'] = float(norm(new_h[:3, 3] - old_h[:3, 3]))
            original = to4(*vm.source_mounts[side])
            before_mount = old_h @ original
            after_mount = worlds[1][FRAME_NODES[side]]
            values['neutral_mount_rotation_delta_degrees'] = float(np.degrees(np.arccos(np.clip((np.trace(before_mount[:3, :3].T @ after_mount[:3, :3]) - 1.) / 2., -1., 1.))))
            values['neutral_mount_translation_delta_units'] = float(norm(before_mount[:3, 3] - after_mount[:3, 3]))
            row['sides'][side] = values
        report['rows'].append(row)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__': main()
