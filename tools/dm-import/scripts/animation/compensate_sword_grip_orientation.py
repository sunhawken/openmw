#!/usr/bin/env python3
"""Restore a readable blade orientation after a wrist pose adaptation.

The blade turns around a fixed in-Hand grip centre; its world translation is not
kept.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from retarget_kf import norm
from audit_staff_mesh_grip import nif, sample_channels
from audit_viewmodel_continuity import tracks
from retarget_kf import matrix_quat, quat_matrix


def pose(channels, time, name):
    quaternion, position = sample_channels({name: channels[name]}, time)[name]
    return quat_matrix(quaternion[[1, 2, 3, 0]]), position


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('before-wrist', 'input', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--grip-point', type=float, nargs=3, default=[0., -1., 0.])
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    point = np.asarray(args.grip_point)
    shutil.copytree(args.input, args.output)
    report = dict(status='applied',
        before_wrist=str(args.before_wrist), input=str(args.input), output=str(args.output),
        model_grip_point=point.tolist(), old_world_translation_preserved=False,
        all_hand_finger_arm_keys_preserved=True, clocks_and_event_keys_preserved=True, files=[])
    outputs = {}
    for group in ('idle1h', 'weapononehand'):
        relative = Path('Animations/brutal_fp') / (group + '.kf')
        old, current = tracks(args.before_wrist / relative), tracks(args.input / relative)
        scene = nif.NiStream(); scene.load(args.input / relative)
        names = [e.string_data for e in scene.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        controllers = dict(zip(names, scene.root.controllers)); target = controllers['Weapon Bone']
        maximum, changed = 0., 0
        rotations = target.data.rotations.keys.copy(); translations = target.data.translations.keys.copy()
        for index, row in enumerate(rotations):
            time = float(row[0]); before, _ = pose(old, time, 'Bip01 R Hand'); after, _ = pose(current, time, 'Bip01 R Hand')
            if np.max(abs(before - after)) < 1e-8:
                continue
            mount, offset = pose(old, time, 'Weapon Bone')
            grip = mount @ point + offset
            new_mount = after.T @ before @ mount
            new_offset = grip - new_mount @ point
            value = matrix_quat(new_mount)
            if value @ row[1:] < 0:
                value *= -1
            rotations[index, 1:] = value
            where = np.flatnonzero(abs(translations[:, 0] - time) < 1e-6)
            if len(where) != 1:
                raise ValueError('Item rotation/translation clock grids differ')
            translations[where[0], 1:] = new_offset
            maximum = max(maximum, float(norm(new_offset - offset)))
            changed += 1
        target.data.rotations.keys = rotations; target.data.translations.keys = translations
        if changed:
            scene.save(args.output / relative)
        outputs[relative.as_posix()] = hashlib.sha256((args.output / relative).read_bytes()).hexdigest()
        after_channels = tracks(args.output / relative)
        for name in current:
            if name == 'Weapon Bone':
                continue
            if any(a.tobytes() != b.tobytes() for a, b in zip(current[name], after_channels[name])):
                raise ValueError('Non-item channel changed: ' + name)
        report['files'].append(dict(path=relative.as_posix(), changed_item_keys=changed,
            maximum_hand_local_mount_translation_change=maximum, sha256=outputs[relative.as_posix()]))
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text()); manifest['outputs'].update(outputs)
    manifest['native_handshake_grip'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'native-handshake-grip.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
