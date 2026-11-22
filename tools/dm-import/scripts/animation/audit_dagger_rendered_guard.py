#!/usr/bin/env python3
"""Compare hand/mount bases and blade endpoints to the raw dagger curves.

Arm world bases and source clocks are measured from the same frame. The guard
item clock follows Dark Messiah: reverse-fast on entry, then forward-slow .03
seconds after lowering. No parry-out MDL events survive the active-layer handoff
to left_idle.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from retarget_kf import norm
from import_weapon_families import Family


def right_grip_profile(vm, declaration):
    if not declaration:
        return None
    reference = vm.item_matrix('@right_down_to_up', 1000.)
    reverse = (vm.weapon_bone[0] @ vm.to_weapon @ vm.item_matrix('@right_up_to_down', 1000.)
               @ reference.T @ vm.to_weapon.T)
    return dict(anchor=vm.weapon_bone[1] + reverse[:, 1] * declaration['anchor_reverse_y'],
                hilt_y=declaration['hilt_y'])


def mount_translation(base, side, rotation, profile):
    return (profile['anchor'] - rotation[:, 1] * profile['hilt_y']
            if side == 'R' and profile else base[1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', type=Path)
    parser.add_argument('--data', required=True, type=Path)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    vm = Family(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    manifest = json.loads((args.data / 'weapon-families-import.json').read_text())['dagger']
    groups = manifest['groups']['brutal_fp_dagger']
    grip_profile = right_grip_profile(vm, manifest.get('right_grip_mount'))
    anatomy = manifest.get('hand_anatomy')
    anatomy_grip = manifest.get('anatomy_grip_mount', {})
    lower = next(time for time, key in groups['brutal_block']['marks'] if key == 'lower start')
    frames = [json.loads(line) for line in (args.profile / 'events.jsonl').read_text().splitlines()
              if json.loads(line)['event'] == 'frame']
    forward = vm.item_matrix('@right_down_to_up', 1000.)
    result = dict(samples=0, max_degrees=0., max_blade_units=0., max_mount_translation_units=0.,
                  max_grip_anchor_units=0., max_native_anchor_units=0., phases={}, errors=[])
    for index, frame in enumerate(frames):
        if not index or frame.get('block') != 'guard-entry-hold-out':
            continue
        source = frame['sources']['left']
        if source['group'] != 'brutal_block':
            continue
        clock = source['time']
        phase = 'entry' if clock < .1 else 'hold' if clock < lower else 'exit'
        for side, hand, mount, base, model in [
            ('L', 'leftHand', 'leftWeapon', vm.left_mount, 'left'),
            ('R', 'rightHand', 'weapon', vm.weapon_bone, 'primary'),
        ]:
            if side == 'R':
                clip, elapsed = '@right_up_to_down_fast', 1000.
            elif clock < lower:
                clip, elapsed = '@left_up_to_down_fast', clock
            elif clock - lower < .03:
                clip, elapsed = '@left_up_to_down_fast', 1000.
            else:
                clip, elapsed = '@left_down_to_up', clock - lower - .03
            delta = vm.to_weapon @ vm.item_matrix(clip, elapsed) @ forward.T @ vm.to_weapon.T
            expected = delta if anatomy else base[0] @ delta
            h, w = frame['bones'][hand], frame['bones'][mount]
            if anatomy:
                actual_hand = h
                h = frame['bones']['leftDaggerFrame' if side == 'L' else 'rightDaggerFrame']
                anchor_native = anatomy_grip.get(side, {}).get('anchor_native_hand')
                if anchor_native is not None:
                    basis = np.column_stack([actual_hand[axis] for axis in 'xyz'])
                    local_anchor = np.linalg.solve(basis, np.asarray(h['position']) - actual_hand['position'])
                    result['max_native_anchor_units'] = max(result['max_native_anchor_units'], float(norm(local_anchor - anchor_native)))
            hand_matrix = np.column_stack([h[axis] for axis in 'xyz'])
            mount_matrix = np.column_stack([w[axis] for axis in 'xyz'])
            u, _, vt = np.linalg.svd(np.linalg.solve(hand_matrix, mount_matrix))
            actual = u @ vt
            expected_translation = (-expected @ np.asarray(anatomy_grip.get(side, {}).get('hilt_point', [0., 0., 0.]))
                                    if anatomy else mount_translation(base, side, expected, grip_profile))
            actual_translation = np.linalg.solve(hand_matrix, np.asarray(w['position']) - h['position'])
            translation_error = float(norm(actual_translation - expected_translation))
            result['max_mount_translation_units'] = max(result['max_mount_translation_units'], translation_error)
            if side == 'R' and grip_profile and not anatomy:
                anchor_error = float(norm(actual_translation + actual[:, 1] * grip_profile['hilt_y']
                                                    - grip_profile['anchor']))
                result['max_grip_anchor_units'] = max(result['max_grip_anchor_units'], anchor_error)
            if anatomy and side in anatomy_grip:
                anchor_error = float(norm(actual_translation + actual @ np.asarray(anatomy_grip[side]['hilt_point'])))
                result['max_grip_anchor_units'] = max(result['max_grip_anchor_units'], anchor_error)
            error = float(np.degrees(np.arccos(np.clip((np.trace(actual.T @ expected) - 1.) / 2., -1., 1.))))
            maximum = 0.
            if model in frame.get('mesh', {}):
                mesh = frame['mesh'][model]
                for endpoint in ('tip', 'base'):
                    local = np.asarray(mesh['local' + endpoint.title()]) * ([-1., 1., 1.] if side == 'L' else [1., 1., 1.])
                    predicted = np.asarray(h['position']) + hand_matrix @ (expected_translation + expected @ local)
                    maximum = max(maximum, float(norm(predicted - mesh[endpoint])))
            label = side + '-' + phase
            row = result['phases'].setdefault(label, dict(samples=0, max_degrees=0., max_blade_units=0.))
            row['samples'] += 1
            row['max_degrees'] = max(row['max_degrees'], error)
            row['max_blade_units'] = max(row['max_blade_units'], maximum)
            result['samples'] += 1
            result['max_degrees'] = max(result['max_degrees'], error)
            result['max_blade_units'] = max(result['max_blade_units'], maximum)
            if (error > .1 or maximum > .04 or translation_error > .02) and len(result['errors']) < 10:
                result['errors'].append(dict(frame=frame['frame'], phase=label, clock=clock, degrees=error,
                                            blade_units=maximum, translation_units=translation_error))
    if result['samples'] < 10:
        result['errors'].append('Too few actual game poses')
    if result['max_native_anchor_units'] > .02:
        result['errors'].append(dict(native_anchor_units=result['max_native_anchor_units']))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(bool(result['errors']))


if __name__ == '__main__':
    main()
