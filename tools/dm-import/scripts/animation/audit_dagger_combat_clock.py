#!/usr/bin/env python3
"""Check emitted dagger clocks against the raw MDL samples.

Uses actual float32 KF times, independent of the converter's sampling grid.
Arm crossfades are excluded; their settled source tails are checked separately.
Every independent item primitive is checked between its emitted keys as well.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from audit_viewmodel_continuity import tracks
from audit_weapon_family_fingers import source_chains, angle, pose_at
from import_viewmodel import Viewmodel, BASIS
from import_weapon_families import Family, ITEM_ANIMATIONS, LEFT_WEAPON
from retarget_kf import norm, quat_matrix


def quaternion_at(keys, time):
    high = min(int(np.searchsorted(keys[:, 0], time, side='right')), len(keys) - 1)
    low = max(0, high - 1)
    span = float(keys[high, 0] - keys[low, 0])
    fraction = min(max((time - keys[low, 0]) / span, 0.), 1.) if span else 0.
    a, b = keys[low, 1:].astype(float), keys[high, 1:].astype(float)
    a /= norm(a); b /= norm(b)
    if a @ b < 0: b = -b
    theta = np.arccos(np.float64(min(max(a @ b, -1.), 1.)))
    q = ((np.sin((1 - fraction) * theta) * a + np.sin(fraction * theta) * b) / np.sin(theta)
         if theta > 1e-7 else (1 - fraction) * a + fraction * b)
    return quat_matrix(q[[1, 2, 3, 0]])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    vm = Family(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    groups = json.loads((args.data / 'weapon-families-import.json').read_text())['dagger']['groups']['brutal_fp_dagger']
    report = dict(arm_samples=0, phalanges=0, item_samples=0, max_finger_degrees=0.,
                  max_item_degrees=0., source_tails=[], errors=[])
    item_model = ITEM_ANIMATIONS['dagger']
    root_bone = vm.dm_bones(item_model)[0].name
    reference = Viewmodel.dm_world(vm, item_model, '@right_down_to_up', 1000.)[root_bone][:3, :3]
    for group, info in groups.items():
        channels = tracks(args.data / 'Animations/brutal_fp_dagger' / (group + '.kf'))
        times = channels['Camera'][0][:, 0]
        for segment in info['segments']:
            for index, time in enumerate(times):
                local = float(time) - segment['offset']
                if local < max(0., segment['fade_in']) - 1e-5 or local > segment['duration'] - segment['return_fade'] + 1e-5:
                    continue
                tail = segment.get('source_tail')
                model, clip = segment['model'], segment['clip']
                if tail and local >= tail['start'] - 1e-5:
                    if local < tail['start'] + tail['fade'] - 1e-5:
                        continue
                    model, clip, raw_time = tail['model'], tail['clip'], local - tail['start']
                    report['source_tails'].append(dict(group=group, time=local, clip=clip))
                else:
                    natural = segment['natural']
                    if segment['loop']:
                        period = segment.get('loop_period') or segment['duration']
                        if segment.get('loop_lead_in', 0.):
                            if local > 1e-4 and abs(local / period - round(local / period)) < 1e-5:
                                continue  # modulo at a float32 seam has two valid neighbours
                            raw_time = (local % period) * natural / period
                        else:
                            raw_time = local * natural / (segment['duration'] - segment['return_fade'])
                    else:
                        raw_time = min(max(local, 0.), natural)
                    raw_time *= segment.get('source_rate', 1.)
                dm = Viewmodel.dm_world(vm, model, clip, max(0., raw_time))
                world = vm.fk(pose_at(channels, index))
                report['arm_samples'] += 1
                for side, finger, chain in source_chains():
                    for joint, suffix in enumerate(('', '1', '2')):
                        native = world[f'Bip01 {side} Finger{finger}{suffix}'][:3, 0]
                        expected = BASIS @ (dm[chain[joint + 1]][:3, 3] - dm[chain[joint]][:3, 3])
                        degrees = angle(native, expected)
                        if degrees > report['max_finger_degrees']:
                            report['max_finger_degrees'] = degrees
                            report['worst_finger'] = dict(group=group,clip=clip,local=local,raw_time=raw_time, side=side,finger=finger,joint=joint)
                        report['phalanges'] += 1
                        if degrees > .1 and len(report['errors']) < 20:
                            report['errors'].append(dict(group=group, clip=clip, local=local,
                                side=side, finger=finger, joint=joint, degrees=degrees))
        for primitive in info['dagger_item_primitives']:
            node, base = ((LEFT_WEAPON, vm.left_mount) if primitive['side'] == 'L'
                          else ('Weapon Bone', vm.weapon_bone))
            for fraction in np.linspace(0, 1, 61):
                time = primitive['start'] + fraction * (primitive['stop'] - primitive['start'])
                actual = quaternion_at(channels[node][0], time)
                raw = Viewmodel.dm_world(vm, item_model, primitive['source_clip'],
                    fraction * Viewmodel.duration(vm, item_model, primitive['source_clip']))[root_bone][:3, :3]
                primitive_base = np.eye(3) if primitive.get('basis') == 'mount-local' else base[0]
                expected = primitive_base @ vm.to_weapon @ raw @ reference.T @ vm.to_weapon.T
                degrees = float(np.degrees(np.arccos(np.clip((np.trace(actual.T @ expected) - 1.) / 2., -1., 1.))))
                report['max_item_degrees'] = max(report['max_item_degrees'], degrees)
                report['item_samples'] += 1
                if degrees > .1 and len(report['errors']) < 20:
                    report['errors'].append(dict(group=group, primitive=primitive, fraction=float(fraction), degrees=degrees))
    if not report['arm_samples'] or not report['item_samples']:
        report['errors'].append('Missing arm or item samples')
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'source_tails'}, indent=2))
    raise SystemExit(bool(report['errors']))


if __name__ == '__main__':
    main()
