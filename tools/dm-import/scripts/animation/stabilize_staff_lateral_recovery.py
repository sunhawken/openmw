#!/usr/bin/env python3
"""Remove the extra open-palm flourish from the native lateral polearm return.

Writes a fresh output directory. After the second lateral hit the left arm takes
one shortest-path return to its final grip, instead of releasing, turning the
wrist and catching the shaft twice. The two strikes, right arm, weapon, camera,
all events and all sample clocks stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from retarget_kf import norm
from audit_staff_mesh_grip import slerp
from audit_viewmodel_continuity import tracks
from audit_dagger_anatomy import sample
from retarget_kf import matrix_quat
from es3 import nif


ARM = {f'Bip01 L {part}' for part in ('Clavicle', 'UpperArm', 'Forearm', 'Hand')}
FINGERS = {f'Bip01 L Finger{i}{suffix}' for i in range(5) for suffix in ('', '1', '2')}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def smooth(value):
    value = np.clip(value, 0., 1.)
    return value * value * (3. - 2. * value)


def patch(source, output):
    if output.exists():
        raise ValueError('A fresh output directory is required: ' + str(output))
    relative = Path('Animations/brutal_fp_staff/weapononehand.kf')
    original = source / relative
    old = nif.NiStream(); old.load(original)
    labels = [(float(time), str(label)) for time, label in old.root.extra_data.keys]
    marks = {label.removeprefix('weapononehand: '): time for time, label in labels}
    channels = tracks(original)
    clocks = channels['Bip01 L Hand'][0][:, 0]
    # Work on the emitted key grid; no boundary insertion or retiming.
    start = float(clocks[np.searchsorted(clocks, marks['slash hit2'] + .025)])
    end = marks['slash large follow stop']
    first = sample(channels, start)
    final = sample(channels, end)
    # Dark Messiah opens the hand after the second impact. Retain an open hand
    # for the return, then close once as the actual moving shaft arrives.
    opened = sample(channels, marks['slash min hit'] + 1.20)
    shutil.copytree(source, output)
    stream = nif.NiStream(); stream.load(output / relative)
    names = [extra.string_data for extra in stream.root.extra_datas if isinstance(extra, nif.NiStringExtraData)]
    controllers = dict(zip(names, stream.root.controllers))
    changed = 0
    for name in ARM | FINGERS:
        controller = controllers[name]
        rotations = controller.data.rotations.keys.copy()
        positions = controller.data.translations.keys.copy()
        if not np.array_equal(rotations[:, 0], positions[:, 0]):
            raise ValueError('Mismatched rotation/translation grids: ' + name)
        for index in np.flatnonzero((rotations[:, 0] >= start - 1e-6) & (rotations[:, 0] <= end + 1e-6)):
            time = float(rotations[index, 0])
            age = time - start
            duration = end - start
            weight = float(smooth(age / duration))
            if name in ARM:
                a, b = first[name], final[name]
                q = slerp(matrix_quat(a[0]), matrix_quat(b[0]), weight)
                p = (1. - weight) * a[1] + weight * b[1]
            else:
                # Release once; do not reproduce the source's intermediate catch.
                opening = float(smooth(age / .15))
                closure = float(smooth((time - (end - .30)) / .30))
                q_open = slerp(matrix_quat(first[name][0]), matrix_quat(opened[name][0]), opening)
                q = slerp(q_open, matrix_quat(final[name][0]), closure)
                p_open = (1. - opening) * first[name][1] + opening * opened[name][1]
                p = (1. - closure) * p_open + closure * final[name][1]
            rotations[index, 1:] = q
            positions[index, 1:] = p
            changed += 1
        # Keep signs continuous across the entire existing track.
        for index in range(1, len(rotations)):
            if rotations[index - 1, 1:] @ rotations[index, 1:] < 0.:
                rotations[index, 1:] *= -1.
        controller.data.rotations.keys = rotations
        controller.data.translations.keys = positions
    stream.save(output / relative)
    check = nif.NiStream(); check.load(output / relative)
    if labels != [(float(time), str(label)) for time, label in check.root.extra_data.keys]:
        raise ValueError('Gameplay text keys changed')
    after = tracks(output / relative)
    preserved = 0
    for name, (rotations, positions) in channels.items():
        for before, later in zip((rotations, positions), after[name]):
            if not np.array_equal(before[:, 0], later[:, 0]):
                raise ValueError('Channel clocks changed: ' + name)
            if name not in ARM | FINGERS:
                if before.tobytes() != later.tobytes():
                    raise ValueError('Retained channel changed: ' + name)
                preserved += len(before)
            else:
                outside = (before[:, 0] < start - 1e-6) | (before[:, 0] > end + 1e-6)
                # Quaternion signs are equivalent; compare both possible signs.
                if before.shape[1] == 5:
                    delta = np.minimum(norm(before[:, 1:] - later[:, 1:], axis=1),
                                       norm(before[:, 1:] + later[:, 1:], axis=1))
                    if np.max(delta[outside]) > 1e-7:
                        raise ValueError('Outside-range rotation changed: ' + name)
                elif not np.array_equal(before[outside], later[outside]):
                    raise ValueError('Outside-range translation changed: ' + name)
    manifest = json.loads((source / 'weapon-families-import.json').read_text())
    report = dict(source=str(source.resolve()), output=str(output.resolve()),
                  mode='single-direct-left-lateral-return',
                  file=relative.as_posix(), start=start, stop=end,
                  second_hit=marks['slash hit2'],
                  clock='Existing emitted key times; right arm and weapon unchanged',
                  gesture='Two original strikes, one open-hand return, one final catch',
                  changed_left_keys=changed, retained_other_channel_keys=preserved,
                  source_sha256=sha(original), output_sha256=sha(output / relative),
                  recipe_sha256=sha(Path(__file__)), gameplay_keys_unchanged=True,
                  status='applied')
    manifest['outputs'][relative.as_posix()] = report['output_sha256']
    manifest['staff']['lateral_recovery'] = report
    (output / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
    (output / 'staff-lateral-recovery-import.json').write_text(json.dumps(report, indent=2) + '\n')
    report['manifest_sha256'] = sha(output / 'weapon-families-import.json')
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(patch(args.input, args.output), indent=2))


if __name__ == '__main__':
    main()
