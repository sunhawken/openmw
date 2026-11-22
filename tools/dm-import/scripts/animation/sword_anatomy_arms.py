#!/usr/bin/env python3
"""Replace the sword _B arm poses with the anatomical solve.

The Hand-target and native-grip modes let the item path adapt to the native
hand; the compensated-blade modes keep the blade transform.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from import_sword_anatomy import SwordAnatomy
from import_viewmodel import Viewmodel
from audit_staff_mesh_grip import Rig, sample_channels
from audit_viewmodel_continuity import tracks
from retarget_kf import matrix_quat, quat_matrix


def world(rig, pose):
    result = {}
    def get(name):
        if name not in result:
            node = rig.nodes[name]
            rotation, position = pose.get(name, (np.asarray(node.rotation), np.asarray(node.translation)))
            local = np.eye(4); local[:3, :3] = rotation; local[:3, 3] = position
            result[name] = get(rig.parents[name]) @ local if name in rig.parents else local
        return result[name]
    for name in rig.nodes:
        get(name)
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--native-rest-hinge', action='store_true')
    p.add_argument('--rest-relative-wrist', action='store_true')
    p.add_argument('--anatomical-wrist-target', action='store_true')
    p.add_argument('--anatomical-hand-target', action='store_true')
    p.add_argument('--reference-hand-frame', action='store_true')
    p.add_argument('--source-left-fingers', action='store_true',
        help='Diagnostic three-phalange directions only; keep native MCP positions and lengths')
    p.add_argument('--left-mcp-anchor', choices=('actual', 'reference'),
        help='Measured native-palm proportion adaptation; keep the fixed Hand rotation')
    a = p.parse_args()
    if a.output.exists():
        p.error('The output directory must not exist yet')
    shutil.copytree(a.input, a.output)
    declaration = json.loads((a.input / 'viewmodel-import.json').read_text())
    rig = Rig(a.input / 'meshes/brutal_fp/xbase_anim.1st.nif')
    report = dict(input=str(a.input), output=str(a.output), mode='R true Wrist/MCP, compensated blade',
                  status='applied', native_rest_hinge=a.native_rest_hinge,
                  rest_relative_wrist=a.rest_relative_wrist,
                  anatomical_wrist_target=a.anatomical_wrist_target, families={})
    report['anatomical_hand_target'] = a.anatomical_hand_target
    report['reference_hand_frame'] = a.reference_hand_frame
    report['source_left_finger_directions_only'] = a.source_left_fingers
    report['left_mcp_anchor'] = a.left_mcp_anchor
    report['source_fourth_finger_node_skin_rotation'] = 'not transferred; controlled combat diagnostic only'
    if a.anatomical_hand_target:
        report['mode'] = 'Bilateral Source Hand flexion pivots, distal Wrist roll guides, rigid native grips'
    changed_names = {'Bip01 R UpperArm', 'Bip01 R Forearm', 'Bip01 R Hand', 'Weapon Bone'}
    if a.anatomical_wrist_target:
        changed_names.add('Bip01 R Clavicle')
    if a.anatomical_hand_target:
        changed_names.update(f'Bip01 {side} {part}' for side in ('L', 'R')
                             for part in ('Clavicle', 'UpperArm', 'Forearm', 'Hand'))
    if a.source_left_fingers:
        from retarget_contract import CONTRACT
        changed_names.update(node for digit in CONTRACT['sides']['L']['digits'] for node in digit['target'])
    for shield, folder in [(False, 'brutal_fp'), (True, 'brutal_fp_shield')]:
        vm = SwordAnatomy(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation,
            camera_contract=a.camera_contract, native_eye_height=True,
            shield_normal=float(declaration.get('shield_normal', -1)), native_rest_hinge=a.native_rest_hinge)
        vm.rest_relative_wrist = a.rest_relative_wrist
        vm.anatomical_wrist_target = a.anatomical_wrist_target
        vm.anatomical_hand_target = a.anatomical_hand_target
        vm.reference_hand_frame = a.reference_hand_frame
        vm.source_left_fingers = a.source_left_fingers
        vm.left_mcp_anchor = a.left_mcp_anchor
        vm.shield = shield
        profile = declaration['shield_geometry_profile']
        vm.shield_offset = np.asarray(profile['offset_item_axes']); vm.shield_scale = profile['scale']
        model = 'first_animations_shield' if shield else 'first_animations_sword'
        prefix = 'anm_bigslash_sword_' + ('shield' if shield else 'noshield') + '_B'
        relative = Path('Animations') / folder / 'weapononehand.kf'
        stream = vm.nif.NiStream(); stream.load(a.input / relative)
        names = [e.string_data for e in stream.root.extra_datas if isinstance(e, vm.nif.NiStringExtraData)]
        controllers = dict(zip(names, stream.root.controllers))
        extra = next(e for e in stream.root.extra_datas if isinstance(e, vm.nif.NiTextKeyExtraData))
        marks = {str(key).removeprefix('weapononehand: '): float(time) for time, key in extra.keys}
        reference = controllers['Bip01 R Hand'].data.rotations.keys[:, 0]
        roll = {}; armed = vm.stance_pose()
        t1, p1, _ = vm.segment(model, prefix + '_in', start_pose=armed[0], roll_state=roll, runtime_entry=True)
        vm.pending.pop(); wind_roll = dict(roll)
        t2, p2, _ = vm.segment(model, prefix + '_out', start_pose=p1[-1], roll_state=roll)
        vm.pending.pop()
        natural = vm.duration(model, prefix + '_idle'); vm.sequence_fade(model, prefix + '_idle')
        loop = bool(vm.sequences[model][prefix + '_idle'].flags & 1)
        lead = .2 if loop else 0.
        t3, p3, _ = vm.segment(model, prefix + '_idle', duration=natural if loop else max(.2, natural),
            loop=loop, loop_lead_in=lead, roll_state=wind_roll, runtime_entry=True)
        vm.pending.pop()
        sections = [('in', marks['thrust start'], marks['thrust max attack'], p1),
                    ('out', marks['thrust min hit'], marks['thrust large follow stop'], p2),
                    ('hold', marks['thrust hold start'], marks['thrust hold stop'], p3)]
        baseline = tracks(a.input / relative)
        replacements = {}; preserve_count = 0; max_blade = 0.
        for key, start, stop, poses in sections:
            indices = np.flatnonzero((reference >= start - 1e-5) & (reference <= stop + 1e-5))
            if len(indices) != len(poses):
                raise ValueError(f'{folder}/{key}: original grid {len(indices)} != source {len(poses)}')
            for index, pose in zip(indices, poses):
                time = float(reference[index]); sampled = sample_channels(baseline, time)
                old = {name: (quat_matrix(q[[1, 2, 3, 0]]), position) for name, (q, position) in sampled.items()}
                previous = world(rig, old)
                updated = dict(old)
                for name in changed_names - {'Weapon Bone'}:
                    updated[name] = (pose[name][0], pose[name][1] if a.anatomical_wrist_target or a.anatomical_hand_target else old[name][1])
                current = world(rig, updated); hand = current['Bip01 R Hand']; blade = previous['Weapon Bone']
                updated['Weapon Bone'] = (vm.source_weapon_mount if a.anatomical_wrist_target or a.anatomical_hand_target else
                    (hand[:3, :3].T @ blade[:3, :3], hand[:3, :3].T @ (blade[:3, 3] - hand[:3, 3])))
                check = world(rig, updated)['Weapon Bone']
                max_blade = max(max_blade, float(np.max(abs(check - blade))))
                replacements[int(index)] = updated
        for name, controller in controllers.items():
            for field in ('rotations', 'translations'):
                channel = getattr(controller.data, field); original = channel.keys.copy(); after = original.copy()
                if name in changed_names:
                    for index, pose in replacements.items():
                        if field == 'rotations':
                            value = matrix_quat(pose[name][0])
                            if value @ original[index, 1:] < 0:
                                value *= -1
                        else:
                            value = pose[name][1]
                        after[index, 1:] = value
                keep = np.ones(len(original), bool)
                if name in changed_names:
                    keep[list(replacements)] = False
                if original[keep].tobytes() != after[keep].tobytes():
                    raise ValueError('Unrelated keys changed')
                preserve_count += int(keep.sum()); channel.keys = after
        stream.save(a.output / relative)
        report['families'][folder] = dict(sections=[dict(key=k, start=s, stop=e, samples=len(ps))
            for k, s, e, ps in sections], preserved_channel_keys=preserve_count,
            max_key_blade_matrix_delta=max_blade,
            preserves_old_blade_world=not (a.anatomical_wrist_target or a.anatomical_hand_target),
            sha256=hashlib.sha256((a.output / relative).read_bytes()).hexdigest())
    manifest_path = a.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text())
    for folder, info in report['families'].items():
        manifest['outputs'][(Path('Animations') / folder / 'weapononehand.kf').as_posix()] = info['sha256']
    manifest['sword_anatomy'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (a.output / 'sword-anatomy.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
