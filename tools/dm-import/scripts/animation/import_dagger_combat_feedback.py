#!/usr/bin/env python3
"""Import Dark Messiah's dagger combat and independent item sequences.

Arm speed comes from daggers.qct/first_basic.qct. Item primitives retain their
own authored clock. Text keys let the native dagger callback keep that clock
through interrupted attacks, guards and recoveries without an arm crossfade.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

from import_dagger_guard_feedback import DaggerGuard
from import_viewmodel import Viewmodel
from import_weapon_families import ITEM_ANIMATIONS, LEFT_WEAPON
from retarget_kf import matrix_quat


class DaggerCombat(DaggerGuard):
    def source_rate(self, model, clip):
        return 1.1 if (model == 'first_animations_daggers' and (
            (clip.startswith('anm_fastattack_daggers_') and '_end_' not in clip)
            or (clip.startswith('anm_bigslash_Daggers') and clip.endswith('_out')))) else 1.

    def preceding_clip(self, clip):
        prefix = 'anm_fastattack_daggers_'
        if clip.startswith(prefix):
            suffix = clip[len(prefix):]
            if suffix in 'BCDE' and len(suffix) == 1:
                return prefix + chr(ord(suffix) - 1)
            if suffix.startswith('end_'):
                return prefix + suffix[-1]
        if clip.startswith('anm_transition_daggers_'):
            return prefix + clip[-1]
        return super().preceding_clip(clip)

    def dagger_state(self, side, clip, time):
        before, plan = self.dagger_timeline(side, clip)
        time += before
        target = 'reverse' if side == 'R' else 'forward'
        active = None
        for finish, duration, requested in plan:
            start = finish - duration
            if start > time + 1e-8:
                break
            if requested == target and (side == 'L' or active is None or active[1] == duration):
                continue
            target = requested
            active = start, duration, target
        if active is not None and time < active[0] + active[1]:
            start, duration, target = active
            name = '@' + ('left' if side == 'L' else 'right') + '_' + (
                'up_to_down' if target == 'reverse' else 'down_to_up') + ('_fast' if duration < .1 else '')
            return self.item_matrix(name, max(0., time - start)) @ self.item_matrix('@right_down_to_up', 1e3).T
        return self.dagger_grip(target)

    def raw_pose(self, model, clip, time):
        self.source_clip = model, clip, time
        dm = self.dm_world(model, clip, time)
        return self.finish_pose(self.solve(dm, {}, weapon_grip=True), dm)

    def default_mount(self, pose, elapsed):
        reference = self.item_matrix('@right_down_to_up', 1e3)
        state = self.item_matrix('@left_down_to_up', max(0., elapsed)) @ reference.T
        pose[LEFT_WEAPON] = (self.left_mount[0] @ self.to_weapon @ state @ self.to_weapon.T,
                            self.left_mount[1])

    def segment(self, model, clip, *args, **kwargs):
        guard_out = clip == 'anm_parry_Daggers_out' and kwargs.get('return_to')
        end_recovery = clip in ('anm_fastattack_daggers_end_C', 'anm_fastattack_daggers_end_D')
        charged_out = clip.startswith('anm_bigslash_Daggers') and clip.endswith('_out')
        if guard_out or charged_out:
            kwargs.pop('return_to', None)
        if guard_out:
            kwargs['duration'] = .03
        times, poses, error = super().segment(model, clip, *args, **kwargs)
        info = self.pending[-1]
        info.update(source_rate=self.source_rate(model, clip),
                    source_natural=Viewmodel.duration(self, model, clip))
        if not (guard_out or end_recovery):
            return times, poses, error
        tail_start = .03 if guard_out else self.duration(model, clip)
        idle_model, idle_clip = 'first_animations_basic', 'anm_daggers_left_idle'
        tail_duration = self.duration(idle_model, idle_clip)
        fade = self.sequence_fade(idle_model, idle_clip)
        tail = np.linspace(0, tail_duration, max(2, round(tail_duration * 30) + 1))[1:]
        start_pose = kwargs.get('start_pose')
        last = poses[-1]
        for elapsed in tail:
            time = tail_start + float(elapsed)
            outgoing = self.raw_pose(model, clip, min(time, self.duration(model, clip))) if guard_out else last
            if guard_out and start_pose is not None and time < self.sequence_fade(model, clip):
                outgoing = self.blend_pose(start_pose, outgoing, time / self.sequence_fade(model, clip))
            target = self.raw_pose(idle_model, idle_clip, float(elapsed))
            pose = self.blend_pose(outgoing, target, min(float(elapsed) / fade, 1.))
            self.default_mount(pose, float(elapsed))
            poses.append(pose)
        times = np.r_[times, tail_start + tail]
        info.update(return_fade=0., source_tail=dict(model=idle_model, clip=idle_clip,
            start=tail_start, duration=tail_duration, fade=fade, outgoing_continues=bool(guard_out)),
            item_default_start=tail_start)
        return times, poses, error

    def write(self, out, group, segments, textkeys):
        # End eligibility refers to .6 of the authored end, before the separate
        # source left-idle recovery. Quick clicks during that tail are ignored.
        textkeys = list(textkeys)
        for (offset, _, _), metadata in zip(segments, self.pending):
            if metadata['clip'].startswith('anm_fastattack_daggers_end_'):
                label = 'fastend' + metadata['clip'][-1].lower()
                textkeys.append((offset + .6 * metadata['natural'], label + ' restart'))
                textkeys.append((offset + metadata['natural'], label + ' restart stop'))
        # Bypass the guard-only writer: every dagger file carries exact
        # source primitives, and the native callback owns the independent clock.
        super(DaggerGuard, self).write(out, group, segments, textkeys)

    def annotate_items(self, output, manifest):
        folder = 'brutal_fp_dagger'
        for group, info in manifest['dagger']['groups'][folder].items():
            path = output / 'Animations' / folder / (group + '.kf')
            stream = self.nif.NiStream(); stream.load(path)
            controllers = list(stream.root.controllers)
            names = [entry.string_data for entry in stream.root.extra_datas
                     if isinstance(entry, self.nif.NiStringExtraData)]
            extra = next(entry for entry in stream.root.extra_datas
                         if isinstance(entry, self.nif.NiTextKeyExtraData))
            marks = [(float(time), str(label)) for time, label in extra.keys
                     if ': dagger ' not in str(label)]
            commands = []
            for segment in info['segments']:
                clip, offset = segment['clip'], segment['offset']
                item_offset = offset
                if clip == 'anm_parry_Daggers_idle':
                    # The raw arm entry is .133333 seconds; its baked .2 arm
                    # fade must not postpone the separate item's idle events.
                    item_offset = Viewmodel.duration(self, self.config['model'], 'anm_parry_Daggers_in')
                for side in 'LR':
                    for finish, duration, target in self.dagger_plan(side, clip):
                        local = finish - duration
                        tail = segment.get('source_tail')
                        if tail and tail.get('outgoing_continues') and local > tail['start'] + 1e-8:
                            # The old parry-out layer keeps fading visually, but
                            # event dispatch follows the new active left-idle layer.
                            continue
                        if local <= segment['duration'] + 1e-6:
                            commands.append(dict(time=item_offset + local, side=side, direction=target,
                                                 speed='fast' if duration < .1 else 'slow',
                                                 keep_direction=(side == 'R' and clip == 'anm_parry_Daggers_in')))
                if 'item_default_start' in segment:
                    for side, direction in [('L', 'forward'), ('R', 'reverse')]:
                        commands.append(dict(time=offset + segment['item_default_start'], side=side,
                                             direction=direction, speed='default'))
            if (group in ('idle', 'idle1h', 'brutal_sprint', 'brutal_sprint1h')
                    or group.startswith(('walk', 'run', 'sneak'))):
                for side, direction in [('L', 'forward'), ('R', 'reverse')]:
                    commands.append(dict(time=0., side=side, direction=direction, speed='default'))
            if group == 'weapononehand':
                equip = next(float(time) for time, label in info['marks'] if label == 'equip start')
                for side in 'LR':
                    commands.append(dict(time=equip, side=side, direction=None, speed='reset'))
            for command in commands:
                side = 'left' if command['side'] == 'L' else 'right'
                action = ((command['direction'] + ' ') if command['direction'] else '') + command['speed']
                if command.get('keep_direction'):
                    action += ' keep direction'
                marks.append((command['time'], group + ': dagger item ' + side + ' ' + action))
            stop = max(time for time, label in marks if label == group + ': stop')
            primitive_start = stop + 1.
            primitives = []
            for side, node, base in [('L', LEFT_WEAPON, self.left_mount), ('R', 'Weapon Bone', self.weapon_bone)]:
                controller = controllers[names.index(node)]
                original = controller.data.rotations.keys
                original = original[original[:, 0] <= stop + 1e-5]
                rows = [row.copy() for row in original]
                cursor = primitive_start
                for direction in ('forward', 'reverse'):
                    for speed in ('slow', 'fast'):
                        clip = '@' + ('left' if side == 'L' else 'right') + '_' + (
                            'down_to_up' if direction == 'forward' else 'up_to_down') + (
                            '_fast' if speed == 'fast' else '')
                        length = self.duration(ITEM_ANIMATIONS['dagger'], clip)
                        clocks = np.linspace(0, length, max(2, int(np.ceil(length * 960)) + 1))
                        for clock in clocks:
                            state = self.item_matrix(clip, float(clock)) @ self.item_matrix('@right_down_to_up', 1e3).T
                            rotation = base[0] @ self.to_weapon @ state @ self.to_weapon.T
                            rows.append(np.r_[cursor + clock, matrix_quat(rotation)])
                        label = group + ': dagger primitive ' + ('left ' if side == 'L' else 'right ') + direction + ' ' + speed
                        marks.extend([(cursor, label + ' start'), (cursor + length, label + ' stop')])
                        primitives.append(dict(side=side, direction=direction, speed=speed,
                                               start=cursor, stop=cursor + length, source_clip=clip))
                        cursor += length + .1
                rows = np.asarray(rows, np.float32)
                for index in range(1, len(rows)):
                    if rows[index - 1, 1:] @ rows[index, 1:] < 0:
                        rows[index, 1:] *= -1
                controller.data.rotations.keys = rows
                controller.stop_time = cursor
            extra.keys = np.asarray(sorted(marks, key=lambda row: row[0]), dtype='<f,O')
            stream.save(path)
            info['dagger_item_commands'] = sorted(commands, key=lambda row: row['time'])
            info['dagger_item_primitives'] = primitives
            info['dagger_item_clock_version'] = 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    vm = DaggerCombat(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                      camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    folder = args.output / 'Animations' / vm.config['folder']
    errors = [vm.weapon(folder), vm.parry_set(folder)]
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['dagger']['groups'][folder.name].update(vm.groups[folder.name])
    vm.annotate_items(args.output, manifest)
    report = dict(converter=Path(__file__).name, max_error=max(errors),
                  arm_rate=1.1, item_rate=1., item_clock_version=1,
                  outputs={path.relative_to(args.output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in sorted(folder.glob('*.kf'))})
    manifest['dagger']['combat_feedback'] = {key: value for key, value in report.items() if key != 'outputs'}
    manifest.setdefault('outputs', {}).update(report['outputs'])
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'dagger-combat-feedback-import.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
