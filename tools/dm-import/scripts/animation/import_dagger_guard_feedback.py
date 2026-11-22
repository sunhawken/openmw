#!/usr/bin/env python3
"""Reproduce the independent item clock of Dark Messiah's dagger guard.

Dark Messiah asks for both reverse grips with the fast flag while entering the
guard, before the MDL idle events. Leaving the guard restores the default grip
through the item's own slow sequence. Only dagger guard assets are replaced;
staff, sword, attacks and the camera skeleton remain untouched.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from import_weapon_families import Family, LEFT_WEAPON, ITEM_ANIMATIONS
from retarget_kf import matrix_quat


class DaggerGuard(Family):
    def dagger_plan(self, side, clip):
        plan = super().dagger_plan(side, clip)
        if clip == self.config['parry'] + '_in':
            # Dark Messiah flips the left and right daggers as the parry begins,
            # before the idle's event 2119. The
            # duplicate slow event cannot restart the already selected grip.
            item = '@' + ('left' if side == 'L' else 'right') + '_up_to_down_fast'
            duration = self.duration(ITEM_ANIMATIONS['dagger'], item)
            return [(duration, duration, 'reverse')] + plan
        return plan

    def segment(self, model, clip, *args, **kwargs):
        times, poses, error = super().segment(model, clip, *args, **kwargs)
        if clip != self.config['parry'] + '_out' or not kwargs.get('return_to'):
            return times, poses, error
        # The default reset is itemSet(4), without the fast flag. Do not rotate
        # the mount by the arm's generic .2-second return-to-idle crossfade.
        item = '@left_down_to_up'
        reset = self.duration(model, clip)
        length = self.duration(ITEM_ANIMATIONS['dagger'], item)
        stop = max(float(times[-1]), reset + length)
        if stop > times[-1] + 1e-6:
            times = np.r_[times, stop]
            poses = poses + [dict(poses[-1])]
        reference = self.item_matrix('@right_down_to_up', 1e3)
        for time, pose in zip(times, poses):
            if time < reset:
                state = self.dagger_state('L', clip, float(time))
            else:
                state = self.item_matrix(item, min(float(time) - reset, length)) @ reference.T
            pose[LEFT_WEAPON] = (
                self.left_mount[0] @ self.to_weapon @ state @ self.to_weapon.T,
                self.left_mount[1],
            )
        self.pending[-1].update(
            item_return_sequence=item, item_return_start=reset,
            item_return_duration=length,
            item_return_reason='Dark Messiah itemSet(4), independent of arm fade.',
        )
        return times, poses, error

    def write(self, out, group, segments, textkeys):
        super().write(out, group, segments, textkeys)
        if group != 'brutal_block':
            return
        # Preserve the .041667-second source item sequence endpoint explicitly:
        # a common 30-Hz arm grid would otherwise smear it until .066667 seconds.
        info = self.groups[out.name][group]
        path = out / (group + '.kf')
        stream = self.nif.NiStream()
        stream.load(path)
        names = [e.string_data for e in stream.root.extra_datas
                 if isinstance(e, self.nif.NiStringExtraData)]
        controllers = list(stream.root.controllers)
        for side, name, base in (('L', LEFT_WEAPON, self.left_mount), ('R', 'Weapon Bone', self.weapon_bone)):
            controller = controllers[names.index(name)]
            rows = controller.data.rotations.keys
            extra = [1 / 60, self.duration(ITEM_ANIMATIONS['dagger'], '@left_up_to_down_fast')]
            lower = info['segments'][-1]['offset']
            reset = self.duration(self.config['model'], self.config['parry'] + '_out')
            extra += [lower + reset, lower + reset + self.duration(
                ITEM_ANIMATIONS['dagger'], '@left_down_to_up')]
            times = sorted(set(np.asarray(list(rows[:, 0]) + extra, np.float32).tolist()))
            rotations = []
            for time in times:
                segment = max((s for s in info['segments'] if s['offset'] <= time + 1e-6),
                              key=lambda s: s['offset'])
                local = max(0., time - segment['offset'])
                if side == 'L' and segment['clip'] == self.config['parry'] + '_out' and local >= reset:
                    reference = self.item_matrix('@right_down_to_up', 1e3)
                    state = self.item_matrix('@left_down_to_up', local - reset) @ reference.T
                else:
                    state = self.dagger_state(side, segment['clip'], local)
                rotation = base[0] @ self.to_weapon @ state @ self.to_weapon.T
                rotations.append(matrix_quat(rotation))
            rotations = np.asarray(rotations, np.float32)
            for index in range(1, len(rotations)):
                if rotations[index - 1] @ rotations[index] < 0:
                    rotations[index] *= -1
            controller.data.rotations.keys = np.column_stack([times, rotations]).astype(np.float32)
        stream.save(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--attack-reference', type=Path,
                        help='Optional existing attacks import with byte-identical dagger KFs; update its metadata')
    args = parser.parse_args()
    vm = DaggerGuard(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                     camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation
    vm.configure('dagger')
    folder = args.output / 'Animations' / vm.config['folder']
    folder.mkdir(parents=True, exist_ok=True)
    error = vm.parry_set(folder)
    report = dict(converter='import_dagger_guard_feedback.py', max_error=error,
                  source_calls=dict(enter=['0x10000', '0x10008'], exit='4'),
                  groups=vm.groups, clips=vm.provenance)
    report['outputs'] = {path.relative_to(args.output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted(folder.iterdir())
                         if path.stem in ('brutal_block', 'brutal_blockhit1', 'brutal_blockhit2',
                                          'brutal_blockhit3', 'brutal_parrybreak')}
    (args.output / 'dagger-guard-feedback-import.json').write_text(json.dumps(report, indent=2) + '\n')
    manifest = args.output / 'weapon-families-import.json'
    if manifest.exists():
        existing = json.loads(manifest.read_text())
        if args.attack_reference:
            reference = args.attack_reference / 'weapon-families-attacks-import.json'
            attacks = json.loads(reference.read_text())['dagger']
            for folder, groups in attacks['groups'].items():
                for group, metadata in groups.items():
                    relative = Path('Animations') / folder / (group + '.kf')
                    old, actual = args.attack_reference / relative, args.output / relative
                    if not old.is_file() or old.read_bytes() != actual.read_bytes():
                        raise ValueError('Attack audit metadata has no byte-identical source: ' + relative.as_posix())
                    existing['dagger']['groups'].setdefault(folder, {})[group] = metadata
            existing['dagger']['attack_audit_reference'] = dict(
                manifest=str(reference), sha256=hashlib.sha256(reference.read_bytes()).hexdigest(),
                verified='All referenced dagger KFs are byte-identical.')
        for folder, groups in vm.groups.items():
            existing['dagger']['groups'].setdefault(folder, {}).update(groups)
        existing.setdefault('outputs', {}).update(report['outputs'])
        existing['dagger']['guard_feedback_converter'] = report['converter']
        manifest.write_text(json.dumps(existing, indent=1) + '\n')


if __name__ == '__main__':
    main()
