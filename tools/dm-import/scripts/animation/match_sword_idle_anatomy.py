#!/usr/bin/env python3
"""Match the bare sword idle to the anatomical _B frames.

Only idle1h arm, finger and item channels change. Existing _B, other KFs, key
clocks, camera and body keys and blend rules stay untouched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from import_sword_anatomy import SwordAnatomy
from retarget_contract import CONTRACT
from retarget_kf import matrix_quat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'profile', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    profile = json.loads(args.profile.read_text())
    vm = SwordAnatomy(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
        camera_contract=args.camera_contract, native_eye_height=True, anatomical_hand_target=True,
        reference_hand_frame=True, source_left_fingers=True, left_mcp_anchor='actual')
    changed = {node for side in CONTRACT['sides'].values() for digit in side['digits'] for node in digit['target']}
    changed.update(f'Bip01 {side} {name}' for side in 'LR' for name in ('Clavicle', 'UpperArm', 'Forearm', 'Hand'))
    changed.add('Weapon Bone')
    relative = Path('Animations/brutal_fp/idle1h.kf')
    scene = vm.nif.NiStream(); scene.load(args.input / relative)
    names = [e.string_data for e in scene.root.extra_datas if isinstance(e, vm.nif.NiStringExtraData)]
    controllers = dict(zip(names, scene.root.controllers))
    times, poses, error = vm.segment('first_animations_basic', 'anm_sword_noshield_idle', loop=True)
    vm.pending.pop()
    for pose in poses:
        for node, rotation in profile['local_rotations'].items():
            pose[node] = np.asarray(rotation), pose[node][1]
        pose['Weapon Bone'] = np.asarray(profile['mount_R']), np.asarray(profile['mount_T'])
    reference_times = controllers['Bip01 R Hand'].data.rotations.keys[:, 0]
    if len(times) != len(reference_times) or np.max(abs(times - reference_times)) > 1e-5:
        raise ValueError('Idle clock/grid differs from the existing source loop')
    untouched = 0
    for name, controller in controllers.items():
        if name not in changed:
            untouched += len(controller.data.rotations.keys) + len(controller.data.translations.keys)
            continue
        for field in ('rotations', 'translations'):
            original = getattr(controller.data, field).keys.copy(); after = original.copy()
            for index, pose in enumerate(poses):
                value = matrix_quat(pose[name][0]) if field == 'rotations' else pose[name][1]
                if field == 'rotations' and value @ original[index, 1:] < 0:
                    value *= -1
                after[index, 1:] = value
            getattr(controller.data, field).keys = after
    shutil.copytree(args.input, args.output)
    scene.save(args.output / relative)
    if (args.input / 'Animations/brutal_fp/weapononehand.kf').read_bytes() != (args.output / 'Animations/brutal_fp/weapononehand.kf').read_bytes():
        raise ValueError('Controlled _B payload changed')
    report = dict(status='applied',
        scope='Bare Sword idle1h only: common arm/Hand frames, actual-MCP L anchor, Source L idle gesture, same R native grip',
        source_model='first_animations_basic', source_clip='anm_sword_noshield_idle', samples=len(poses),
        unchanged_non_arm_channel_keys=untouched, existing_weapon_payload_byte_preserved=True,
        profile_sha256=hashlib.sha256(args.profile.read_bytes()).hexdigest(),
        sha256=hashlib.sha256((args.output / relative).read_bytes()).hexdigest())
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text()); manifest['outputs'][relative.as_posix()] = report['sha256']
    manifest['sword_matching_idle'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'sword-matching-idle.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
