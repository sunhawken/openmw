#!/usr/bin/env python3
"""Bake one coupled polearm return after the second forward impact.

The shaft returns once; the left hand clears the right hand laterally before
approaching its final grip and closing once. Smooth shoulder translations while
retaining the endpoints, local arm origins, lengths and weapon mount. All
playable data outside this post-impact interval remains exact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from retarget_kf import cross, norm
from audit_staff_mesh_grip import Rig, slerp
from audit_viewmodel_continuity import angle, tracks
from retarget_kf import matrix_quat
from stabilize_staff_left_windup import axial, load, mix, smooth, swing, transport_rotation, unit

REL = Path('Animations/brutal_fp_staff/weapononehand.kf')


def name(side, bone):
    return f'Bip01 {side} {bone}'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch(source, output, settle_seconds=.2, lateral_duration=.2, close_distance=6.):
    if output.exists():
        raise ValueError('The output directory must not exist yet')
    from es3 import nif
    before = tracks(source / REL)
    stream = load(source / REL)
    labels = [(float(t), str(k)) for t, k in stream.root.extra_data.keys]
    marks = {k.removeprefix('weapononehand: '): t for t, k in labels}
    grid = before[name('L', 'Hand')][0][:, 0]
    first = int(np.argmin(abs(grid - (marks['chop hit2'] + settle_seconds))))
    assert grid[first] > marks['chop hit2'] + .04
    last = int(np.argmin(abs(grid - marks['chop large follow stop'])))
    start, stop = float(grid[first]), float(grid[last])
    rig = Rig(source / 'meshes/brutal_fp/xbase_anim.1st.nif')
    A, B = rig.world(before, first), rig.world(before, last)
    LH, RH = name('L', 'Hand'), name('R', 'Hand')
    relative_grip = np.linalg.inv(B[RH]) @ B[LH]
    weapon_mount = np.linalg.inv(A[RH]) @ A['Weapon Bone']
    assert np.max(abs(weapon_mount - np.linalg.inv(B[RH]) @ B['Weapon Bone'])) < 1e-5
    left_start, left_stop = A[LH][:3, 3], B[LH][:3, 3]
    grip_start = (A[RH] @ relative_grip)[:3, 3]
    open_index = int(np.argmin(abs(grid - 4.4)))
    opening_duration = .15
    open_world = rig.world(before, open_index)
    open_relative = np.linalg.inv(open_world['Weapon Bone']) @ open_world[LH]
    assert norm(open_relative[[0, 2], 3]) > 20, 'Free-hand finger reference required'
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = dict(zip(names, stream.root.controllers))
    references, changed = {}, set()
    for side in ('L', 'R'):
        U, F, H, C = [name(side, bone) for bone in ('UpperArm', 'Forearm', 'Hand', 'Clavicle')]
        a_axis, b_axis = [unit(v[H][:3, 3] - v[U][:3, 3]) for v in (A, B)]

        def elbow_pole(v, axis):
            bend = v[F][:3, 3] - v[U][:3, 3]
            return unit(bend - axis * np.dot(bend, axis))

        a_bend, b_bend = elbow_pole(A, a_axis), elbow_pole(B, b_axis)
        transported = swing(a_axis, b_axis) @ a_bend
        roll = np.arctan2(np.dot(b_axis, cross(transported, b_bend)), np.dot(transported, b_bend))
        references[side] = a_axis, a_bend, roll
        changed.add((C, 'translations'))
        changed.update((bone, 'rotations') for bone in (U, F, H))
        for digit in range(5):
            for suffix in ('', '1', '2'):
                changed.update((name(side, f'Finger{digit}{suffix}'), field)
                               for field in ('rotations', 'translations'))
    for bone, field in changed:
        assert np.array_equal(getattr(controllers[bone].data, field).keys[:, 0], grid), bone + ' shared key grid'
    targets, clamps, previous, receipts, weapon_goals = [], [], {}, [], []
    for i in range(first, last + 1):
        t = float(grid[i])
        progress = smooth((t - start) / (stop - start))
        world = rig.world(before, i)
        left = (1 - progress) * left_start + progress * left_stop
        lateral = smooth((t - start) / lateral_duration)
        left[0] = (1 - lateral) * left_start[0] + lateral * left_stop[0]
        grip = (1 - progress) * grip_start + progress * left_stop
        right_rotation = mix(A[RH][:3, :3], B[RH][:3, :3], progress)
        right = grip - right_rotation @ relative_grip[:3, 3]
        rotations = {'L': mix(A[LH][:3, :3], B[LH][:3, :3], progress), 'R': right_rotation}
        right_goal = np.eye(4)
        right_goal[:3, :3], right_goal[:3, 3] = right_rotation, right
        weapon_goals.append((i, right_goal @ weapon_mount))
        for side, target in [('L', left), ('R', right)]:
            U, F, H, C = [name(side, bone) for bone in ('UpperArm', 'Forearm', 'Hand', 'Clavicle')]
            shoulder_root = (1 - progress) * A[C][:3, 3] + progress * B[C][:3, 3]
            parent = world[rig.parents[C]]
            assert np.max(abs(parent - A[rig.parents[C]])) < 1e-5, C + ' moving parent'
            controllers[C].data.translations.keys[i, 1:] = parent[:3, :3].T @ (shoulder_root - parent[:3, 3])
            shoulder = world[U][:3, 3] + shoulder_root - world[C][:3, 3]
            upper_length, fore_length = [float(norm(before[bone][1][i, 1:])) for bone in (F, H)]
            delta = target - shoulder
            distance = float(norm(delta))
            axis = delta / max(distance, 1e-12)
            bounded = float(np.clip(distance, abs(upper_length - fore_length) + 1e-5,
                                    upper_length + fore_length - 1e-5))
            clamps.append(abs(distance - bounded))
            target = shoulder + axis * bounded
            a_axis, a_bend, roll = references[side]
            bend = axial(axis, progress * roll) @ swing(a_axis, axis) @ a_bend
            along = (upper_length**2 - fore_length**2 + bounded**2) / (2 * bounded)
            elbow = shoulder + axis * along + bend * np.sqrt(max(0, upper_length**2 - along**2))
            upper = transport_rotation(A[U][:3, :3], B[U][:3, :3],
                                       unit(A[F][:3, 3] - A[U][:3, 3]),
                                       unit(B[F][:3, 3] - B[U][:3, 3]), unit(elbow - shoulder), progress)
            fore = transport_rotation(A[F][:3, :3], B[F][:3, :3],
                                      unit(A[H][:3, 3] - A[F][:3, 3]),
                                      unit(B[H][:3, 3] - B[F][:3, 3]), unit(target - elbow), progress)
            for bone, rotation in {U: world[C][:3, :3].T @ upper,
                                   F: upper.T @ fore, H: fore.T @ rotations[side]}.items():
                q = matrix_quat(rotation)
                if bone in previous and previous[bone] @ q < 0:
                    q = -q
                previous[bone] = q
                controllers[bone].data.rotations.keys[i, 1:] = q
            targets.append((i, H, target.copy(), shoulder.copy()))
        gap = float(norm(grip - left))
        closure = smooth(1 - gap / close_distance)
        for side in ('L', 'R'):
            if side == 'L' and t - start < opening_duration:
                finger_first, finger_last = first, open_index
                weight = smooth((t - start) / opening_duration)
            elif side == 'L':
                finger_first, finger_last, weight = open_index, last, closure
            else:
                finger_first, finger_last, weight = first, last, progress
            for digit in range(5):
                for suffix in ('', '1', '2'):
                    bone = name(side, f'Finger{digit}{suffix}')
                    controllers[bone].data.rotations.keys[i, 1:] = slerp(
                        before[bone][0][finger_first, 1:], before[bone][0][finger_last, 1:], weight)
                    controllers[bone].data.translations.keys[i, 1:] = (
                        (1 - weight) * before[bone][1][finger_first, 1:]
                        + weight * before[bone][1][finger_last, 1:])
        receipts.append(dict(t=t, progress=progress, lateral_progress=lateral,
                             grip_gap=gap, left_closure=closure))
    for bone, field in changed:
        j = 0 if field == 'rotations' else 1
        keys = getattr(controllers[bone].data, field).keys
        keys[first], keys[last] = before[bone][j][first], before[bone][j][last]
    shutil.copytree(source, output)
    stream.save(output / REL)
    after = tracks(output / REL)
    assert labels == [(float(t), str(k)) for t, k in load(output / REL).root.extra_data.keys]
    for bone, pair in before.items():
        for field, old, new in zip(('rotations', 'translations'), pair, after[bone]):
            assert np.array_equal(old[:, 0], new[:, 0]), bone + ' clocks'
            keep = np.ones(len(old), bool)
            if (bone, field) in changed:
                keep[first + 1:last] = False
            assert np.array_equal(old[keep], new[keep]), bone + ' outside return'
    errors, shoulder_errors, worlds = [], [], {}
    for i, bone, target, shoulder in targets:
        if i not in worlds:
            worlds[i] = rig.world(after, i)
        errors.append(float(norm(worlds[i][bone][:3, 3] - target)))
        upper = bone.replace('Hand', 'UpperArm')
        shoulder_errors.append(float(norm(worlds[i][upper][:3, 3] - shoulder)))
    assert max(errors) < 1e-4
    assert max(clamps) < 1e-5
    assert max(shoulder_errors) < 1e-4
    weapon_error = max(float(np.max(abs(worlds[i]['Weapon Bone'] - goal))) for i, goal in weapon_goals)
    assert weapon_error < 1e-4
    open_finger_distance = sum(float(angle(before[bone][0][open_index:open_index + 1, 1:],
                                          before[bone][0][last:last + 1, 1:])[0])
                               for bone in before if bone.startswith('Bip01 L Finger'))
    assert open_finger_distance > 300, 'Open native fingers required'
    report = dict(source=str(source.resolve()), output=str(output.resolve()), file=REL.as_posix(),
                  start=start, stop=stop, first=first, last=last,
                  mode='one-open-early-lateral-clearance-and-single-final-catch',
                  settle_seconds=settle_seconds, lateral_duration=lateral_duration, close_distance=close_distance,
                  max_weapon_target_error=weapon_error,
                  open_finger_source_time=float(grid[open_index]), opening_duration=opening_duration,
                  open_finger_distance_degrees=open_finger_distance,
                  max_reach_clamp=max(clamps), max_wrist_target_error=max(errors),
                  max_shoulder_path_error=max(shoulder_errors),
                  shoulder_endpoints_exact=True, arm_origins_and_lengths_unchanged=True,
                  preparation_damage_hold_lateral_wall_and_event_clocks_exact=True,
                  weapon_mount_exact=all(np.array_equal(a, b) for a, b in zip(before['Weapon Bone'], after['Weapon Bone'])),
                  source_sha256=sha(source / REL), output_sha256=sha(output / REL), recipe_sha256=sha(Path(__file__)),
                  status='applied')
    manifest = json.loads((source / 'weapon-families-import.json').read_text())
    manifest['outputs'][REL.as_posix()] = report['output_sha256']
    manifest['staff']['forward_return'] = report
    (output / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
    (output / 'staff-forward-return-import.json').write_text(json.dumps(report, indent=2) + '\n')
    (output / 'staff-forward-return-targets.json').write_text(json.dumps(receipts, indent=2) + '\n')
    return report


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--settle-seconds', type=float, default=.2)
    p.add_argument('--lateral-duration', type=float, default=.2)
    p.add_argument('--close-distance', type=float, default=6.)
    args = p.parse_args()
    print(json.dumps(patch(args.input, args.output, args.settle_seconds, args.lateral_duration,
                           args.close_distance), indent=2))
