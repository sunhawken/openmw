#!/usr/bin/env python3
"""Apply a native grip profile to the bare sword _B sections.

The profile holds absolute native finger-local rotations and a rigid Hand-local
item mount, measured from the meshes of the user's game.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from audit_staff_mesh_grip import nif
from retarget_kf import matrix_quat
from retarget_contract import CONTRACT


def rotation(value):
    value = np.asarray(value, float)
    if value.shape != (3, 3) or np.max(abs(value.T @ value - np.eye(3))) > 1e-4 or abs(np.linalg.det(value) - 1) > 1e-4:
        raise ValueError('Profile contains a non-rotation')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'profile'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    profile = json.loads(args.profile.read_text())
    expected = {node for digit in CONTRACT['sides']['R']['digits'] for node in digit['target']}
    if set(profile['local_rotations']) != expected:
        raise ValueError('Profile must explicitly supply all 15 right native phalanges')
    local = {name: rotation(value) for name, value in profile['local_rotations'].items()}
    local['Weapon Bone'] = rotation(profile['mount_R'])
    translation = np.asarray(profile['mount_T'], float)
    if translation.shape != (3,) or not np.isfinite(translation).all():
        raise ValueError('Invalid hand-local item translation')
    relative = Path('Animations/brutal_fp/weapononehand.kf')
    scene = nif.NiStream(); scene.load(args.input / relative)
    names = [e.string_data for e in scene.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = dict(zip(names, scene.root.controllers))
    if not set(local) <= controllers.keys():
        raise ValueError('Selected KF lacks explicit grip channels')
    extra = next(e for e in scene.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))
    original_marks = [(float(time), str(key)) for time, key in extra.keys]
    marks = {key.removeprefix('weapononehand: '): time for time, key in original_marks}
    ranges = [('in', marks['thrust start'], marks['thrust max attack']),
              ('out', marks['thrust min hit'], marks['thrust large follow stop']),
              ('hold', marks['thrust hold start'], marks['thrust hold stop'])]
    before = {}
    changed = 0
    for name, controller in controllers.items():
        for field in ('rotations', 'translations'):
            channel = getattr(controller.data, field)
            original = channel.keys.copy(); before[name, field] = original
            if name not in local or (field == 'translations' and name != 'Weapon Bone'):
                continue
            after = original.copy()
            mask = np.zeros(len(after), bool)
            for label, start, stop in ranges:
                mask |= (after[:, 0] >= start - 1e-5) & (after[:, 0] <= stop + 1e-5)
            for index in np.flatnonzero(mask):
                if field == 'rotations':
                    value = matrix_quat(local[name])
                    if value @ original[index, 1:] < 0:
                        value *= -1
                else:
                    value = translation
                after[index, 1:] = value
                changed += 1
            if original[~mask].tobytes() != after[~mask].tobytes():
                raise ValueError('Unrelated section changed')
            channel.keys = after
    shutil.copytree(args.input, args.output)
    scene.save(args.output / relative)
    check = nif.NiStream(); check.load(args.output / relative)
    after_names = [e.string_data for e in check.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    for name, controller in zip(after_names, check.root.controllers):
        for field in ('rotations', 'translations'):
            after = getattr(controller.data, field).keys
            original = before[name, field]
            if after[:, 0].tobytes() != original[:, 0].tobytes():
                raise ValueError('Source key clocks changed')
            if name not in local or (field == 'translations' and name != 'Weapon Bone'):
                if after.tobytes() != original.tobytes():
                    raise ValueError('Unrelated node/translation changed')
    after_marks = next(e for e in check.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))
    if original_marks != [(float(time), str(key)) for time, key in after_marks.keys]:
        raise ValueError('Event keys changed')
    report = dict(status='applied',
        scope='Bare Sword _B only: fixed native R finger pose and rigid Hand-local mount',
        source_profile=str(args.profile), profile_sha256=hashlib.sha256(args.profile.read_bytes()).hexdigest(),
        source_profile_status=profile.get('status'), source_profile_measurements=profile,
        input=str(args.input), output=str(args.output), changed_channel_keys=changed,
        native_local_translations_except_item_preserved=True,
        arm_joint_world_positions_and_rotations_preserved=True, source_clocks_and_event_keys_preserved=True,
        sha256=hashlib.sha256((args.output / relative).read_bytes()).hexdigest())
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['outputs'][relative.as_posix()] = report['sha256']
    manifest['sword_grip_profile'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'sword-grip-profile.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('status', 'scope', 'changed_channel_keys', 'sha256')}, indent=2))


if __name__ == '__main__':
    main()
