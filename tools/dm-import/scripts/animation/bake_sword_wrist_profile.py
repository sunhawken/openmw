#!/usr/bin/env python3
"""Bake a native wrist preference into a complete sword dataset.

By default it covers the bare sword idle and the charged _B sections. Other
families and all animation clocks, markers, local finger and item transforms
stay unchanged.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from audit_staff_mesh_grip import Rig, nif
from native_wrist_profile import lateral_cap
from retarget_anatomical_arm import frame
from retarget_kf import matrix_quat, quat_matrix


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--limit', type=float, default=30.)
    parser.add_argument('--halfwidth', type=float, default=5.)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    if not (args.input / 'meshes/brutal_fp/xbase_anim.1st.nif').is_file():
        parser.error('Input must be a complete firstperson dataset, not a KF overlay')
    rig = Rig(args.input / 'meshes/brutal_fp/xbase_anim.1st.nif')
    palms = {}
    for side in 'LR':
        points = np.asarray([rig.nodes[f'Bip01 {side} Finger{i}'].translation for i in range(1, 5)], float)
        palms[side] = frame(points.mean(0), points[3] - points[0])
    shutil.copytree(args.input, args.output)
    report = dict(status='applied',
        input=str(args.input), output=str(args.output), limit=args.limit, transition_halfwidth=args.halfwidth,
        local_fingers_and_item_mount_preserved=True, hand_origin_preserved=True,
        source_world_blade_intentionally_changes=True, files=[])
    outputs = {}
    for group in ('idle1h', 'weapononehand'):
        relative = Path('Animations/brutal_fp') / (group + '.kf')
        scene = nif.NiStream(); scene.load(args.input / relative)
        names = [e.string_data for e in scene.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        controllers = dict(zip(names, scene.root.controllers))
        extra = next(e for e in scene.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))
        marks = {str(key).removeprefix(group + ': '): float(time) for time, key in extra.keys}
        ranges = [(marks['thrust start'], marks['thrust max attack']),
                  (marks['thrust min hit'], marks['thrust large follow stop']),
                  (marks['thrust hold start'], marks['thrust hold stop'])] if group == 'weapononehand' else []
        changed = {}
        for side in 'LR':
            controller = controllers[f'Bip01 {side} Hand']
            original = controller.data.rotations.keys.copy(); after = original.copy()
            positions = controller.data.translations.keys
            count, maximum = 0, 0.
            for index, row in enumerate(original):
                time = float(row[0])
                if group == 'weapononehand' and not any(start - 1e-5 <= time <= stop + 1e-5 for start, stop in ranges):
                    continue
                rotation = quat_matrix(row[1:][[1, 2, 3, 0]])
                translation = np.asarray([np.interp(time, positions[:, 0], positions[:, i]) for i in range(1, 4)])
                correction, amount = lateral_cap(rotation, translation, palms[side], args.limit, args.halfwidth)
                maximum = max(maximum, abs(amount))
                if abs(amount) <= 1e-7:
                    continue
                value = matrix_quat(rotation @ correction)
                if value @ row[1:] < 0:
                    value *= -1
                after[index, 1:] = value; count += 1
            controller.data.rotations.keys = after
            changed[side] = dict(rotation_keys=count, maximum_correction_degrees=maximum)
        if any(row['rotation_keys'] for row in changed.values()):
            scene.save(args.output / relative)
        else:
            shutil.copyfile(args.input / relative, args.output / relative)
        outputs[relative.as_posix()] = hashlib.sha256((args.output / relative).read_bytes()).hexdigest()
        report['files'].append(dict(path=relative.as_posix(), changes=changed, sha256=outputs[relative.as_posix()]))
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text()); manifest['outputs'].update(outputs)
    manifest['native_sword_wrist_profile'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'native-sword-wrist-profile.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
