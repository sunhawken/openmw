#!/usr/bin/env python3
"""Inspect emitted viewmodel KF keys for temporal joint flips.

This catches forearm/hand compensation that keeps the IK hand endpoint correct while
twisting the visible forearm. Connected source phases must meet without a pose snap;
independently played equip/unequip/attack ranges are reported separately. Loop seams
must also meet. With --baseline and --skeleton, also check that a twist correction
preserves the world hand position and orientation at every emitted key.
"""
import argparse
import json
from pathlib import Path
import sys
import numpy as np

from retarget_kf import norm
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'third_party/io_scene_mw/lib'))
from es3 import nif
from retarget_kf import quat_matrix


def tracks(path):
    stream = nif.NiStream(); stream.load(path)
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    return {name: (c.data.rotations.keys, c.data.translations.keys)
            for name, c in zip(names, stream.root.controllers)}


def scale_tracks(path):
    stream = nif.NiStream(); stream.load(path)
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    return {name: c.data.scales.keys for name, c in zip(names, stream.root.controllers)}


def angle(a, b):
    a = a/norm(a, axis=-1, keepdims=True)
    b = b/norm(b, axis=-1, keepdims=True)
    return np.degrees(2*np.arccos(np.clip(np.abs(np.sum(a*b, axis=-1)), 0, 1)))


def world_hands(nodes, parents, channels, index):
    world = {}
    def get(name):
        if name not in world:
            node = nodes[name]
            matrix = np.eye(4)
            if name in channels:
                rotations, positions = channels[name]
                q = rotations[index, 1:]
                matrix[:3, :3] = quat_matrix(q[[1, 2, 3, 0]])
                matrix[:3, 3] = positions[index, 1:]
            else:
                matrix[:3, :3] = node.rotation
                matrix[:3, 3] = node.translation
            world[name] = get(parents[name])@matrix if name in parents else matrix
        return world[name]
    return [get('Bip01 '+side+' Hand') for side in ('R', 'L')]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path, help='Imported data with viewmodel-import.json')
    parser.add_argument('--max-step-degrees', type=float, default=120,
                        help='Conservative diagnostic limit, not a claim of smooth animation')
    parser.add_argument('--baseline', type=Path, help='Another emitted set for endpoint comparison')
    parser.add_argument('--skeleton', type=Path, help='Required with --baseline: original xbase_anim.1st.nif')
    parser.add_argument('--output', type=Path, help='Optional JSON diagnostic report')
    args = parser.parse_args()
    if args.baseline and not args.skeleton: parser.error('--baseline requires --skeleton')
    manifest = json.loads((args.data/'viewmodel-import.json').read_text())
    groups = manifest['groups']
    carrier_name = (manifest.get('camera_carrier') or {}).get('node')
    if args.baseline:
        stream = nif.NiStream(); stream.load(args.skeleton)
        nodes = {n.name: n for n in stream.objects_of_type(nif.NiNode) if n.name}
        parents = {c.name: n.name for n in nodes.values() for c in n.children
                   if isinstance(c, nif.NiNode) and c.name}
    rows, errors = [], []
    if carrier_name:
        skeleton = nif.NiStream(); skeleton.load(args.data/'meshes/brutal_fp/xbase_anim.1st.nif')
        carrier = next((node for node in skeleton.root.children
                        if getattr(node, 'name', None) == carrier_name), None)
        if carrier is None or not any(node.name == 'ModelRoot' for node in carrier.children):
            errors.append('Camera carrier must directly wrap the first-person skeleton under FileRoot')
        pivot_name = manifest['camera_contract']['pivot_node']
        if not any(node.name == pivot_name for node in skeleton.root.children):
            errors.append('Static pitch pivot must remain outside the animated camera carrier')
    max_position, max_rotation = 0., 0.
    for folder, clips in sorted(groups.items()):
        for group, info in sorted(clips.items()):
            relative = Path('Animations')/folder/(group+'.kf')
            channels = tracks(args.data/relative)
            if carrier_name:
                carrier_track = channels.get(carrier_name)
                if carrier_track is None:
                    errors.append(f'{relative}: missing camera carrier reset/motion track')
                else:
                    positions = carrier_track[1][:, 1:]
                    if not np.allclose(positions[[0, -1]], 0., atol=1e-5):
                        errors.append(f'{relative}: camera carrier does not return to the neutral stance')
                    extent = float(np.max(norm(positions, axis=1)))
                    if group == 'brutal_kick1h' and extent < 1.:
                        errors.append(f'{relative}: proven kick camera recoil is missing')
                    elif group != 'brutal_kick1h' and extent > 1e-5:
                        errors.append(f'{relative}: unverified camera carrier motion outside kick')
            profile = manifest.get('shield_geometry_profile')
            if profile is not None:
                expected_scale = profile['scale'] if folder.endswith('_shield') else 1.
                keys = scale_tracks(args.data/relative).get('Shield Bone')
                if keys is None or not len(keys):
                    errors.append(f'{relative}: missing Shield Bone scale track')
                elif not np.isfinite(keys).all() or np.any(np.diff(keys[:, 0]) <= 0) or not np.allclose(keys[:, 1], expected_scale, atol=1e-6):
                    errors.append(f'{relative}: invalid Shield Bone scale; expected {expected_scale:.7f}')
            segments = info['segments']
            row = {'set': folder, 'group': group, 'max_step_degrees': 0., 'joins': [],
                   'max_connected_join_degrees': 0., 'max_connected_join_position': 0.,
                   'loop_seam_degrees': 0., 'loop_seam_position': 0.}
            times = next(iter(channels.values()))[0][:, 0]
            joins = np.array([s['offset'] for s in segments[1:]])
            # Only adjacent keys from the same source segment belong to the temporal gate.
            intra = np.ones(len(times)-1, dtype=bool)
            for join in joins: intra &= ~((times[:-1] < join) & (times[1:] >= join-1e-6))
            connected = ~intra & (np.diff(times) < .005)
            marks = {label: time for time, label in info['marks']}
            looping = 'loop start' in marks and 'loop stop' in marks
            if looping:
                loop_first = int(np.argmin(np.abs(times-marks['loop start'])))
                loop_last = int(np.argmin(np.abs(times-marks['loop stop'])))
            for name, (rotations, positions) in channels.items():
                if not np.isfinite(rotations).all() or not np.isfinite(positions).all():
                    errors.append(f'{relative}: nonfinite {name}'); continue
                if np.any(np.diff(rotations[:, 0]) <= 0) or np.any(np.diff(positions[:, 0]) <= 0):
                    errors.append(f'{relative}: nonmonotonic {name}'); continue
                steps = angle(rotations[:-1, 1:], rotations[1:, 1:])
                distances = norm(np.diff(positions[:, 1:], axis=0), axis=1)
                if np.any(connected):
                    row['max_connected_join_degrees'] = max(row['max_connected_join_degrees'], float(np.max(steps[connected])))
                    row['max_connected_join_position'] = max(row['max_connected_join_position'], float(np.max(distances[connected])))
                if looping:
                    row['loop_seam_degrees'] = max(row['loop_seam_degrees'], float(angle(rotations[loop_first, 1:], rotations[loop_last, 1:])))
                    row['loop_seam_position'] = max(row['loop_seam_position'], float(norm(positions[loop_first, 1:]-positions[loop_last, 1:])))
                if np.any(intra):
                    index = np.flatnonzero(intra)[np.argmax(steps[intra])]
                    if steps[index] > row['max_step_degrees']:
                        row.update(max_step_degrees=float(steps[index]), bone=name, time=float(times[index]))
                for index in np.flatnonzero(~intra):
                    if steps[index] > 30:
                        row['joins'].append({'bone': name, 'time': float(times[index]), 'degrees': float(steps[index]),
                                             'connected': bool(connected[index]), 'gap': float(times[index+1]-times[index])})
            if row['max_step_degrees'] > args.max_step_degrees:
                errors.append(f'{relative}: {row["bone"]} turns {row["max_step_degrees"]:.2f} degrees in one key step')
            if row['max_connected_join_degrees'] > .1 or row['max_connected_join_position'] > .001:
                errors.append(f'{relative}: connected phase join changes pose by {row["max_connected_join_degrees"]:.3f} degrees / {row["max_connected_join_position"]:.5f} units')
            if row['loop_seam_degrees'] > .1 or row['loop_seam_position'] > .001:
                errors.append(f'{relative}: loop seam changes pose by {row["loop_seam_degrees"]:.3f} degrees / {row["loop_seam_position"]:.5f} units')
            if args.baseline:
                original = tracks(args.baseline/relative)
                if set(original) != set(channels): raise ValueError(f'Track names differ in {relative}')
                if any(not np.array_equal(original[n][0][:, 0], channels[n][0][:, 0]) for n in channels):
                    raise ValueError(f'Key times differ in {relative}')
                for index in range(len(times)):
                    old = world_hands(nodes, parents, original, index)
                    new = world_hands(nodes, parents, channels, index)
                    for before, after in zip(old, new):
                        max_position = max(max_position, float(norm(before[:3, 3]-after[:3, 3])))
                        # Float32 skeleton bind matrices are slightly nonorthogonal; remove
                        # that common scale/shear before measuring a rotation-only delta.
                        bu, _, bv = np.linalg.svd(before[:3, :3])
                        au, _, av = np.linalg.svd(after[:3, :3])
                        relative_rotation = (bu@bv).T@(au@av)
                        max_rotation = max(max_rotation, float(np.degrees(np.arccos(
                            np.clip((np.trace(relative_rotation)-1)/2, -1, 1)))))
            rows.append(row)
    if max_position > .0001 or max_rotation > .01:
        errors.append(f'World hand pose changed: {max_position:.8f} units / {max_rotation:.8f} degrees')
    report = {'clips': rows, 'max_world_hand_position_change': max_position,
              'max_world_hand_rotation_change_degrees': max_rotation, 'errors': errors}
    if args.output: args.output.write_text(json.dumps(report, indent=2)+'\n')
    for row in sorted(rows, key=lambda r: -r['max_step_degrees'])[:12]:
        print(f'{row["set"]}/{row["group"]}: {row["max_step_degrees"]:.2f} deg; phase joins >30deg: {len(row["joins"])}')
    print(f'{len(rows)} clips; hand pose change {max_position:.8f} units / {max_rotation:.8f} degrees')
    for error in errors: print('FAIL:', error)
    raise SystemExit(bool(errors))


if __name__ == '__main__': main()
