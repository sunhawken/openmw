#!/usr/bin/env python3
"""Regenerate native sword/shield arm poses on the existing playable key grids.

Camera/body tracks, all event clocks, blend rules and non-sword assets remain
unchanged. Source supplies gesture/opening/timing; native hand proportions,
wrist preference and in-hand grip determine the visible pose.
"""
import argparse
import hashlib
import json
from pathlib import Path
import pickle
import shutil

import numpy as np

from retarget_kf import cross, norm

from import_sword_anatomy import SwordAnatomy
from import_viewmodel import BASIS
from native_wrist_profile import lateral_cap
from retarget_anatomical_arm import follow_finger_directions
from retarget_contract import CONTRACT
from retarget_kf import matrix_quat, quat_matrix


class NativeSword(SwordAnatomy):
    def __init__(self, *args, grip_profile, **kwargs):
        super().__init__(*args, anatomical_hand_target=True, reference_hand_frame=True,
                         source_left_fingers=True, left_mcp_anchor='actual', **kwargs)
        self.native_grip = grip_profile
        self.closed_curls = {}
        self.captured = {}
        self.pose_weapon_grip = True

    @staticmethod
    def curls(dm):
        source = {name.casefold(): value for name, value in dm.items()}
        result = []
        for digit in CONTRACT['sides']['R']['digits']:
            if digit['role'] == 'thumb':
                continue
            points = np.asarray([source[name.casefold()][:3, 3] for name in digit['source']])
            directions = np.diff(points, axis=0)
            directions /= norm(directions, axis=1)[:, None]
            result.append(np.arccos(np.clip(np.sum(directions[:-1] * directions[1:], axis=1), -1., 1.)).sum())
        return np.asarray(result)

    def solve(self, dm, roll_state=None, weapon_grip=True):
        clip = getattr(self, 'source_clip', ('', '', 0.))[1]
        self.pose_weapon_grip = weapon_grip and clip not in ('@idle01', '@Megasprint')
        return super().solve(dm, roll_state, self.pose_weapon_grip)

    def finish_pose(self, pose, dm):
        pose = super().finish_pose(pose, dm)
        local = {name: self.local(name) for name in self.nodes}
        gesture = follow_finger_directions(pose, dm, self.fk(pose), local, self.parents, BASIS, 'R')
        if self.shield not in self.closed_curls:
            clip = 'anm_shield_idle' if self.shield else 'anm_sword_noshield_idle'
            self.closed_curls[self.shield] = np.maximum(self.curls(self.dm_world('first_animations_basic', clip, 0.)), .001)
        closure = float(np.mean(self.curls(dm) / self.closed_curls[self.shield]))
        amount = np.clip((closure - .35) / .4, 0., 1.) if self.pose_weapon_grip else 0.
        amount = float(amount * amount * (3 - 2 * amount))
        fitted = dict(gesture)
        for name, rotation in self.native_grip['local_rotations'].items():
            fitted[name] = np.asarray(rotation), gesture[name][1]
        blended = self.blend_pose(gesture, fitted, amount)
        for digit in CONTRACT['sides']['R']['digits']:
            for name in digit['target']:
                pose[name] = blended[name]
        if self.pose_weapon_grip:
            pose['Weapon Bone'] = np.asarray(self.native_grip['mount_R']), np.asarray(self.native_grip['mount_T'])
        if self.shield:
            # Source Leftshield belongs to distal Wrist; native Shield Bone is
            # below Hand. Preserve that attachment independently of palm pose.
            source = dm['SK_L_Wrist'] @ self.shield_attachment
            target_rotation = BASIS @ source[:3, :3] @ self.shield_map
            target_position = self.shield_target(source)
            hand = self.fk(pose)['Bip01 L Hand']
            pose['Shield Bone'] = (hand[:3, :3].T @ target_rotation,
                hand[:3, :3].T @ (target_position - hand[:3, 3]))
        return pose

    def stance_pose(self, armed=True, blocking=False):
        model = ('first_animations_shield' if self.shield else 'first_animations_sword') if blocking else 'first_animations_basic'
        clip = ('anm_parry_sword_shield_idle' if self.shield else 'anm_parry_sword_idle') if blocking else (
            ('anm_shield_idle' if self.shield else 'anm_sword_noshield_idle') if armed else '@idle01')
        dm = self.dm_world(model, clip, 0.)
        self.pose_weapon_grip = armed
        pose = SwordAnatomy.solve(self, dm, {}, weapon_grip=armed)
        return self.finish_pose(pose, dm), self.sequence_fade(model, clip)

    def write(self, out, group, segments, textkeys):
        sources = list(self.pending)
        self.pending = []
        rows = []
        for (offset, times, poses), source in zip(segments, sources):
            labels = [key for time, key in textkeys if abs(time - offset) < 1e-5 and key not in ('start', 'loop start')]
            label = labels[0] if labels else ('start' if abs(offset) < 1e-8 else None)
            rows.append(dict(offset=offset, times=times, poses=poses, source=source, label=label))
        self.captured[group] = rows

    def native_pose(self, pose):
        result = dict(pose)
        for side in 'LR':
            node = f'Bip01 {side} Hand'
            rotation, translation = result[node]
            palm = self.reference_arm_calibration[side]
            points = np.asarray([self.local(f'Bip01 {side} Finger{i}')[1] for i in range(1, 5)])
            from retarget_anatomical_arm import frame
            basis = frame(points.mean(0), points[3] - points[0])
            correction, amount = lateral_cap(rotation, translation, basis)
            if abs(amount) < 1e-7:
                continue
            result[node] = rotation @ correction, translation
            if side == 'R' and 'Weapon Bone' in result:
                mount, offset = result['Weapon Bone']; point = np.array([0., -1., 0.])
                grip = mount @ point + offset
                target = correction.T @ mount
                result['Weapon Bone'] = target, grip - target @ point
            elif side == 'L' and self.shield and 'Shield Bone' in result:
                mount, offset = result['Shield Bone']
                result['Shield Bone'] = correction.T @ mount, correction.T @ offset
        return result


def sample_poses(times, poses, time):
    index = int(np.clip(np.searchsorted(times, time, side='right') - 1, 0, len(times) - 1))
    following = min(index + 1, len(times) - 1)
    duration = float(times[following] - times[index])
    amount = np.clip((time - times[index]) / duration, 0, 1) if duration > 1e-10 else 0.
    return SwordAnatomy.blend_pose(poses[index], poses[following], float(amount))


def generate(vm, folder):
    basic = 'first_animations_basic'
    kind = 'shield' if vm.shield else 'noshield'
    vm.loop(folder, 'idle1h', basic, 'anm_shield_idle' if vm.shield else 'anm_sword_noshield_idle')
    for mode in ('walk', 'run', 'sneak'):
        clip = ('@walk_sword_shield' if vm.shield else '@walk_sword') if mode != 'run' else ('@run_sword_shield' if vm.shield else '@run_sword')
        for direction in ('forward', 'back', 'left', 'right'):
            vm.loop(folder, mode + direction + '1h', basic, clip)
    vm.loop(folder, 'brutal_sprint1h', basic, '@Megasprint_sword_shield' if vm.shield else '@Megasprint_sword')
    vm.weapon(folder)
    vm.kick(folder, basic, f'anm_sword_{kind}_kick_medium')
    vm.kick(folder, basic, f'anm_sword_{kind}_kick_low', 'low')
    vm.rush(folder); vm.fall_attack(folder)
    if vm.shield:
        vm.block(folder)
    else:
        vm.parry(folder)
        vm.loop(folder, 'idle', basic, '@idle01')
        vm.loop(folder, 'brutal_sprint', basic, '@Megasprint')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path)
    for name in ('profile', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--captured', type=Path,
                        help='File holding the generated poses of --folder; reused when present, written otherwise')
    parser.add_argument('--capture-only', action='store_true',
                        help='Only generate and store the poses; needs --shield-mesh instead of --input')
    parser.add_argument('--shield-mesh', type=Path,
                        help='Native shield NIF: measures the shield placement an input set would declare')
    parser.add_argument('--folder', choices=('brutal_fp', 'brutal_fp_shield'),
                        help='Regenerate one set only, so both can run side by side and be merged')
    args = parser.parse_args()
    if args.captured and not args.folder:
        parser.error('--captured stores one set; name it with --folder')
    if args.capture_only and not args.captured:
        parser.error('--capture-only needs --captured')
    if not args.capture_only:
        if args.output is None:
            parser.error('--output is required')
        if args.output.exists():
            parser.error('The output directory must not exist yet')
    profile = json.loads(args.profile.read_text())
    if args.capture_only and args.input is None:
        if args.shield_mesh is None:
            parser.error('--capture-only needs --input or --shield-mesh')
        from import_viewmodel import Viewmodel
        measured = Viewmodel(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation, -1.,
                             args.shield_mesh, True, args.camera_contract, True)
        declaration = dict(shield_normal=-1., shield_geometry_profile=measured.shield_profile)
    elif args.input is None:
        parser.error('--input is required')
    else:
        declaration = json.loads((args.input / 'viewmodel-import.json').read_text())
    if not args.capture_only:
        shutil.copytree(args.input, args.output)
    report = dict(status='applied', files=[],
        scope='Sword/Shield arm/finger/item channels on existing clock grids; hidden intersections allowed',
        profile_sha256=hashlib.sha256(args.profile.read_bytes()).hexdigest())
    outputs = {}
    for shield, folder in ((False, 'brutal_fp'), (True, 'brutal_fp_shield')):
        if args.folder not in (None, folder):
            continue
        vm = NativeSword(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
            grip_profile=profile, camera_contract=args.camera_contract, native_eye_height=True,
            shield_normal=float(declaration.get('shield_normal', -1)))
        vm.shield = shield
        geometry = declaration['shield_geometry_profile']
        vm.shield_offset = np.asarray(geometry['offset_item_axes']); vm.shield_scale = geometry['scale']
        if args.captured and args.captured.is_file():
            vm.captured = pickle.loads(args.captured.read_bytes())
        else:
            generate(vm, Path(folder))
            if args.captured:
                temporary = args.captured.with_name(args.captured.name + '.part')
                temporary.write_bytes(pickle.dumps(vm.captured))
                temporary.replace(args.captured)
        if args.capture_only:
            return
        hold_metadata = declaration['groups'][folder]['weapononehand']['segments']
        for source in hold_metadata:
            if not source['clip'].endswith('_idle') or 'bigslash' not in source['clip']:
                continue
            lead = float(source.get('loop_lead_in', 0.))
            times, poses, _ = vm.segment(source['model'], source['clip'],
                duration=float(source['loop_period']) if source.get('loop') else float(source['duration']),
                loop=bool(source.get('loop')), loop_lead_in=lead, runtime_entry=True, roll_state={})
            vm.pending.pop()
            vm.captured['weapononehand'].append(dict(offset=float(source['offset']), times=times, poses=poses,
                source=source, label=None))
        changed_names = {f'Bip01 {side} {part}' for side in 'LR' for part in ('Clavicle', 'UpperArm', 'Forearm', 'Hand')}
        changed_names |= {node for side in CONTRACT['sides'].values() for digit in side['digits'] for node in digit['target']}
        changed_names |= {'Weapon Bone', 'Shield Bone'}
        for group, sections in vm.captured.items():
            relative = Path('Animations') / folder / (group + '.kf')
            if not (args.input / relative).is_file():
                continue
            scene = vm.nif.NiStream(); scene.load(args.input / relative)
            names = [e.string_data for e in scene.root.extra_datas if isinstance(e, vm.nif.NiStringExtraData)]
            controllers = dict(zip(names, scene.root.controllers))
            extra = next(e for e in scene.root.extra_datas if isinstance(e, vm.nif.NiTextKeyExtraData))
            marks = {str(key).removeprefix(group + ': '): float(time) for time, key in extra.keys}
            for section in sections:
                section['target_offset'] = marks[section['label']] if section['label'] in marks else section['offset']
            times = controllers['Bip01 R Hand'].data.rotations.keys[:, 0]
            baked, unmapped = {}, []
            for index, time in enumerate(times):
                selected = next((section for section in sections if section['target_offset'] - 1e-5 <= time
                                 <= section['target_offset'] + section['times'][-1] + 1e-5), None)
                if selected is None:
                    unmapped.append(float(time)); continue
                local = float(time) - selected['target_offset']
                baked[index] = vm.native_pose(sample_poses(selected['times'], selected['poses'], local))
            if unmapped:
                raise ValueError(f'Unmapped playable/source rows {folder}/{group}: {unmapped[:10]}')
            preserved = 0
            for name, controller in controllers.items():
                for field in ('rotations', 'translations'):
                    channel = getattr(controller.data, field); original = channel.keys.copy(); after = original.copy()
                    if name in changed_names:
                        for index, pose in baked.items():
                            value = matrix_quat(pose[name][0]) if field == 'rotations' else pose[name][1]
                            if field == 'rotations' and value @ original[index, 1:] < 0:
                                value *= -1
                            after[index, 1:] = value
                        if field == 'rotations' and group.startswith('brutal_blockhit'):
                            for index in range(1, len(after)):
                                if after[index - 1, 1:] @ after[index, 1:] < 0:
                                    after[index, 1:] *= -1
                    else:
                        preserved += len(original)
                    channel.keys = after
            if folder == 'brutal_fp' and group == 'weapononehand':
                # The _B sections are kept as they are here.
                baseline = vm.nif.NiStream(); baseline.load(args.input / relative)
                original_controllers = dict(zip(names, baseline.root.controllers))
                frozen_ranges = [(marks['thrust start'], marks['thrust max attack']),
                    (marks['thrust min hit'], marks['thrust large follow stop']),
                    (marks['thrust hold start'], marks['thrust hold stop'])]
                for name, controller in controllers.items():
                    for field in ('rotations', 'translations'):
                        values = getattr(controller.data, field).keys
                        old = getattr(original_controllers[name].data, field).keys
                        frozen = np.zeros(len(values), bool)
                        for start, stop in frozen_ranges:
                            frozen |= (values[:, 0] >= start - 1e-5) & (values[:, 0] <= stop + 1e-5)
                        values[frozen] = old[frozen]
            scene.save(args.output / relative)
            if folder == 'brutal_fp' and group == 'idle1h':
                shutil.copyfile(args.input / relative, args.output / relative)
            outputs[relative.as_posix()] = hashlib.sha256((args.output / relative).read_bytes()).hexdigest()
            report['files'].append(dict(path=relative.as_posix(), samples=len(times), non_arm_keys_preserved=preserved,
                                       sha256=outputs[relative.as_posix()]))
            print(folder, group, len(times), flush=True)
    # The controlled _B sections and the matching idle keep their exact channels.
    report['kept_sections_input'] = str(args.input)
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text()); manifest['outputs'].update(outputs)
    manifest['native_sword_visual_cycle'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'native-sword-visual-cycle.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(files=len(report['files']), status=report['status'])))


if __name__ == '__main__':
    main()
