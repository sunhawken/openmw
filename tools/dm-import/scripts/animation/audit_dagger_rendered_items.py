#!/usr/bin/env python3
"""Replay primary MDL item events against hand and blade bases.

The converter's emitted dagger_item_commands are not read. Event IDs and raw
clocks come from the MDL; the few code-driven requests follow Dark Messiah's
item lifecycle. Arm poses come from the native renderer, so its fades and
interruptions cannot hide a mount rewind.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from retarget_kf import norm
from import_viewmodel import Viewmodel
from import_weapon_families import Family
from audit_dagger_rendered_guard import right_grip_profile, mount_translation


def primary_commands(vm, group, info):
    commands = []
    def add(time, side, target, fast=False, reset=False):
        commands.append((float(np.float32(time)), side, target, fast, reset))
    for segment in info['segments']:
        model, clip, offset = segment['model'], segment['clip'], segment['offset']
        vm.sequence_fade(model, clip)
        sequence = vm.sequences[model].get(clip)
        rate = 1.1 if (model == 'first_animations_daggers' and (
            (clip.startswith('anm_fastattack_daggers_') and '_end_' not in clip)
            or (clip.startswith('anm_bigslash_Daggers') and clip.endswith('_out')))) else 1.
        raw_duration = Viewmodel.duration(vm, model, clip)
        event_offset = (Viewmodel.duration(vm, model, 'anm_parry_Daggers_in')
                        if clip == 'anm_parry_Daggers_idle' else offset)
        for event in sequence.events if sequence else ():
            mapping = {2119: ('L', 'reverse'), 2120: ('L', 'forward'),
                       2121: ('R', 'reverse'), 2122: ('R', 'forward')}
            if event.event not in mapping:
                continue
            local = float(event.cycle) * raw_duration / rate
            if clip == 'anm_parry_Daggers_out' and local > .03:
                continue
            side, target = mapping[event.event]
            add(event_offset + local, side, target, str(event.options).startswith('1'))
        if clip == 'anm_parry_Daggers_in':
            add(offset, 'L', 'reverse', True); add(offset, 'R', 'reverse', True)
        if clip == 'anm_daggers_versus_r_idle':
            add(offset, 'L', 'forward', True); add(offset, 'R', 'forward', True)
        if clip == 'anm_parry_Daggers_out' or clip in ('anm_fastattack_daggers_end_C', 'anm_fastattack_daggers_end_D'):
            when = offset + (.03 if clip == 'anm_parry_Daggers_out' else raw_duration)
            add(when, 'L', 'forward'); add(when, 'R', 'reverse')
    if group in ('idle', 'idle1h', 'brutal_sprint', 'brutal_sprint1h') or group.startswith(('walk', 'run', 'sneak')):
        add(0., 'L', 'forward'); add(0., 'R', 'reverse')
    if group == 'weapononehand':
        when = next(time for time, label in info['marks'] if label == 'equip start')
        add(when, 'L', 'forward', reset=True); add(when, 'R', 'reverse', reset=True)
    return sorted(commands, key=lambda row: row[0])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', type=Path)
    parser.add_argument('--data', required=True, type=Path)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    vm = Family(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    manifest = json.loads((args.data / 'weapon-families-import.json').read_text())['dagger']
    groups = manifest['groups']['brutal_fp_dagger']
    grip_profile = right_grip_profile(vm, manifest.get('right_grip_mount'))
    anatomy = manifest.get('hand_anatomy')
    anatomy_grip = manifest.get('anatomy_grip_mount', {})
    commands = {name: primary_commands(vm, name, info) for name, info in groups.items()}
    frames = [json.loads(line) for line in (args.profile / 'events.jsonl').read_text().splitlines()
              if json.loads(line)['event'] == 'frame']
    state = {side: dict(target='forward' if side == 'L' else 'reverse', fast=False, elapsed=1000.,
                        group=None, time=0.) for side in 'LR'}
    forward = vm.item_matrix('@right_down_to_up', 1000.)
    result = dict(frames=len(frames), samples=0, max_degrees=0., max_blade_units=0.,
                  max_mount_translation_units=0., max_grip_anchor_units=0., max_native_anchor_units=0., blocks={}, errors=[])

    def advance(current, duration):
        current['elapsed'] = min(1000., current['elapsed'] + max(0., duration))

    def consume(current, side, group, begin, end, inclusive=False, budget=None):
        span = max(0., end - begin)
        scale = (budget / span if span > 1e-10 else 0.) if budget is not None else 1.
        cursor = begin
        for time, own, target, fast, reset in commands.get(group, ()):
            if own != side or time < begin or (not inclusive and time == begin) or time > end:
                continue
            advance(current, (time - cursor) * scale); cursor = time
            if reset:
                current.update(target=target, fast=False, elapsed=1000.)
            elif target != current['target']:
                current.update(target=target, fast=fast, elapsed=0.)
        advance(current, (end - cursor) * scale)
        if budget is not None and span <= 1e-10:
            advance(current, budget)

    def loop_bounds(group):
        marks = groups[group]['marks']
        return (next((float(np.float32(t)) for t, key in marks if key == 'loop start'), None),
                next((float(np.float32(t)) for t, key in marks if key == 'loop stop'), None))

    previous_sim = frames[0]['sim'] if frames else 0.
    for frame in frames:
        simulation_step = max(0., frame['sim'] - previous_sim)
        previous_sim = frame['sim']
        for side, source_name, hand_name, mount_name, base, mesh_name in [
            ('L', 'left', 'leftHand', 'leftWeapon', vm.left_mount, 'left'),
            ('R', 'right', 'rightHand', 'weapon', vm.weapon_bone, 'primary'),
        ]:
            source = frame['sources'][source_name]
            group, now = source['group'], source['time']
            if group not in groups:
                continue
            current = state[side]
            old, previous = current['group'], current['time']
            dt = simulation_step
            loop_start, loop_stop = loop_bounds(group)
            if old is None:
                pass  # Recording begins in a settled idle, before any requests.
            elif group == old and -1e-6 <= now - previous <= dt + 1e-4:
                consume(current, side, group, previous, now, budget=dt)
            elif (group == old and loop_start is not None and now < previous
                  and 0 <= loop_stop - previous + now - loop_start <= dt + 1e-4):
                span = loop_stop - previous + now - loop_start
                first = dt * (loop_stop - previous) / span
                consume(current, side, group, previous, loop_stop, budget=first)
                consume(current, side, group, loop_start, now, True, dt - first)
            else:
                marks = [float(np.float32(t)) for t, key in groups[group]['marks']
                         if t <= now + 1e-5 and now - t <= dt + .002]
                start = max(marks) if marks else 0.
                new_elapsed = max(0., now - start)
                new_budget = min(dt, new_elapsed)
                old_budget = max(0., dt - new_budget)
                consume(current, side, old, previous, previous + old_budget, budget=old_budget)
                consume(current, side, group, start, now, True, new_budget)
            current.update(group=group, time=now)
            direction = 'up_to_down' if current['target'] == 'reverse' else 'down_to_up'
            clip = '@' + ('left' if side == 'L' else 'right') + '_' + direction + ('_fast' if current['fast'] else '')
            delta = vm.to_weapon @ vm.item_matrix(clip, current['elapsed']) @ forward.T @ vm.to_weapon.T
            expected = delta if anatomy else base[0] @ delta
            hand, mount = frame['bones'][hand_name], frame['bones'][mount_name]
            if anatomy:
                actual_hand = hand
                hand = frame['bones']['leftDaggerFrame' if side == 'L' else 'rightDaggerFrame']
                anchor_native = anatomy_grip.get(side, {}).get('anchor_native_hand')
                if anchor_native is not None:
                    hand_basis = np.column_stack([actual_hand[axis] for axis in 'xyz'])
                    local_anchor = np.linalg.solve(hand_basis, np.asarray(hand['position']) - actual_hand['position'])
                    anchor_error = float(norm(local_anchor - anchor_native))
                    result['max_native_anchor_units'] = max(result['max_native_anchor_units'], anchor_error)
            h = np.column_stack([hand[axis] for axis in 'xyz'])
            w = np.column_stack([mount[axis] for axis in 'xyz'])
            u, _, vt = np.linalg.svd(np.linalg.solve(h, w))
            actual = u @ vt
            error = float(np.degrees(np.arccos(np.clip((np.trace(actual.T @ expected) - 1.) / 2., -1., 1.))))
            if anatomy:
                fit = anatomy_grip.get(side, {})
                expected_translation = -expected @ np.asarray(fit.get('hilt_point', [0., 0., 0.]))
            else:
                expected_translation = mount_translation(base, side, expected, grip_profile)
            actual_translation = np.linalg.solve(h, np.asarray(mount['position']) - hand['position'])
            translation_error = float(norm(actual_translation - expected_translation))
            result['max_mount_translation_units'] = max(result['max_mount_translation_units'], translation_error)
            if side == 'R' and grip_profile and not anatomy:
                anchor_error = float(norm(actual_translation + actual[:, 1] * grip_profile['hilt_y']
                                                    - grip_profile['anchor']))
                result['max_grip_anchor_units'] = max(result['max_grip_anchor_units'], anchor_error)
            if anatomy and side in anatomy_grip:
                anchor_error = float(norm(actual_translation + actual @ np.asarray(anatomy_grip[side]['hilt_point'])))
                result['max_grip_anchor_units'] = max(result['max_grip_anchor_units'], anchor_error)
            blade = 0.
            mesh = frame.get('mesh', {}).get(mesh_name)
            if mesh:
                for endpoint in ('base', 'tip'):
                    local = np.asarray(mesh['local' + endpoint.title()]) * ([-1., 1., 1.] if side == 'L' else [1., 1., 1.])
                    predicted = np.asarray(hand['position']) + h @ (expected_translation + expected @ local)
                    blade = max(blade, float(norm(predicted - mesh[endpoint])))
            label = frame.get('block', 'idle')
            row = result['blocks'].setdefault(label, dict(samples=0, max_degrees=0., max_blade_units=0.))
            row['samples'] += 1; row['max_degrees'] = max(row['max_degrees'], error); row['max_blade_units'] = max(row['max_blade_units'], blade)
            result['samples'] += 1; result['max_degrees'] = max(result['max_degrees'], error); result['max_blade_units'] = max(result['max_blade_units'], blade)
            if (error > .1 or blade > .04 or translation_error > .02) and len(result['errors']) < 20:
                result['errors'].append(dict(frame=frame['frame'], block=label, side=side, group=group,
                    time=now, primitive=clip, elapsed=current['elapsed'], degrees=error, blade_units=blade))
    if result['max_native_anchor_units'] > .02:
        result['errors'].append(dict(native_anchor_units=result['max_native_anchor_units']))
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(not result['samples'] or bool(result['errors']))


if __name__ == '__main__':
    main()
