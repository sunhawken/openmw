#!/usr/bin/env python3
"""Append Dark Messiah's charged idles without moving existing playable keys.

The native controller selects these sections after ordinary windups and the
fast/guard charge transitions. Source sequence flags determine loops; nonloops
settle and clamp. Entry fades run once on the simulation animation clock.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from import_viewmodel import Viewmodel, FPS
from import_weapon_families import Family
from import_dagger_anatomy import DaggerAnatomy
from retarget_kf import matrix_quat


FOLDERS = {'sword': 'brutal_fp', 'shield': 'brutal_fp_shield',
           'staff': 'brutal_fp_staff', 'dagger': 'brutal_fp_dagger'}
READY = {'sword': 2.5 / 30., 'shield': 4. / 30., 'staff': .03,
         'dagger': .03 + 12.5 / 30.}
ATTACKS = [('neutral', ''), ('chop', '_T'), ('thrust', '_B'),
           ('slash', '_L'), ('slashright', '_R')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def keys_of(stream, nif):
    return next(e for e in stream.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))


def append_channels(stream, nif, sections):
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    preserved = 0
    for name, controller in zip(names, stream.root.controllers):
        for channel, width in [('rotations', 5), ('translations', 4), ('scales', 2)]:
            data = getattr(controller.data, channel)
            original = data.keys.copy()
            if not len(original):
                continue
            rows = []
            for section in sections:
                for time, pose in zip(section['times'], section['poses']):
                    if name not in pose:
                        raise ValueError('Missing new pose channel: ' + name)
                    rotation, translation = pose[name]
                    value = matrix_quat(rotation) if channel == 'rotations' else (
                        translation if channel == 'translations' else original[-1, 1:])
                    rows.append(np.r_[time, value])
            added = np.asarray(rows, np.float32).reshape((-1, width))
            if not np.isfinite(added).all():
                raise ValueError('Nonfinite charged hold keys: ' + name)
            if channel == 'rotations':
                previous = original[np.searchsorted(original[:, 0], added[0, 0]) - 1, 1:]
                for row in added:
                    if previous @ row[1:] < 0:
                        row[1:] *= -1
                    previous = row[1:]
            combined = np.concatenate([original, added])
            combined = combined[np.argsort(combined[:, 0], kind='stable')]
            retained = combined[np.isin(combined[:, 0], original[:, 0])]
            if retained.tobytes() != original.tobytes():
                raise ValueError('Changed existing channel keys: ' + name + ':' + channel)
            data.keys = combined
            preserved += len(original)
        controller.stop_time = max(float(controller.stop_time), float(sections[-1]['times'][-1]))
    return preserved


def patch_family(a, family, manifest, viewmodel):
    folder = FOLDERS[family]
    factory = DaggerAnatomy if family == 'dagger' else Family if family == 'staff' else Viewmodel
    vm = factory(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation,
                 shield_normal=float(viewmodel.get('shield_normal', -1.)),
                 camera_contract=a.camera_contract, native_eye_height=True)
    if family in ('sword', 'shield') and viewmodel.get('shield_geometry_profile'):
        profile = viewmodel['shield_geometry_profile']
        vm.shield_offset = np.asarray(profile['offset_item_axes'], float)
        vm.shield_scale = float(profile['scale'])
        vm.shield_profile = dict(profile)
    vm.grip_path = a.grip_animation
    if family in ('dagger', 'staff'):
        if family == 'staff':
            vm.staff_mount_offset = float(manifest['staff'].get('staff_mount_offset', 20.))
        vm.configure(family)
        model, prefix = vm.config['model'], vm.config['attack']
        metadata = manifest[family]['groups'][folder]['weapononehand']
    else:
        vm.shield = family == 'shield'
        model = 'first_animations_shield' if vm.shield else 'first_animations_sword'
        prefix = 'anm_bigslash_sword_' + ('shield' if vm.shield else 'noshield')
        metadata = viewmodel['groups'][folder]['weapononehand']
    path = a.output / 'Animations' / folder / 'weapononehand.kf'
    stream = vm.nif.NiStream(); stream.load(path)
    extra = keys_of(stream, vm.nif)
    old_marks = [(float(t), str(k)) for t, k in extra.keys]
    marks = dict((k.removeprefix('weapononehand: '), t) for t, k in old_marks)
    if any(' hold start' in key for _, key in old_marks):
        raise ValueError('Charged holds already present: ' + str(path))
    old_stop = marks['stop']
    cursor, sections, added_marks, commands, hit_changes = old_stop + .01, [], [], [], {}
    primitive_start = min((t for t, k in old_marks if ': dagger primitive ' in k), default=float('inf'))
    for attack, suffix in ATTACKS:
        if family in ('dagger', 'staff') and attack == 'slashright':
            continue  # These original families alias both side inputs to _L.
        clip = prefix + suffix + '_idle'
        natural = vm.duration(model, clip)
        if not np.isfinite(natural) or natural <= 0:
            raise ValueError('Invalid source hold duration: ' + clip)
        fade = vm.sequence_fade(model, clip)
        sequence = vm.sequences[model][clip]
        loop = bool(int(sequence.flags) & 1)
        lead = float(np.ceil(max(.2, fade) * FPS - 1e-5) / FPS) if loop else 0.
        length = natural if loop else max(.2, natural, fade)
        times, poses, error = vm.segment(model, clip, duration=length, loop=loop,
            loop_lead_in=lead, runtime_entry=True, roll_state={})
        source = vm.pending.pop()
        stop = cursor + float(times[-1])
        if stop >= primitive_start - 1e-5:
            raise ValueError('New hold overlaps independent item primitives: ' + family)
        row = dict(key=attack, clip=clip, offset=cursor, duration=float(times[-1]),
                   natural=natural, loop=loop, loop_lead=lead, fade=fade,
                   max_hand_error=error, times=times + cursor, poses=poses, source=source)
        sections.append(row)
        added_marks += [(cursor, 'weapononehand: ' + attack + ' hold start'),
                        (stop, 'weapononehand: ' + attack + ' hold stop')]
        if loop:
            added_marks += [(cursor + lead, 'weapononehand: ' + attack + ' hold loop start'),
                            (stop, 'weapononehand: ' + attack + ' hold loop stop'),
                            (cursor + lead, 'weapononehand: loop start'), (stop, 'weapononehand: loop stop')]
        if family == 'dagger':
            for side in 'LR':
                for finish, duration, target in vm.dagger_plan(side, clip):
                    local = finish - duration
                    if local > float(times[-1]) + 1e-6:
                        continue
                    speed = 'fast' if duration < .1 else 'slow'
                    command = dict(time=cursor + local, side=side, direction=target, speed=speed)
                    commands.append(command)
                    label = 'left' if side == 'L' else 'right'
                    added_marks.append((command['time'], f'weapononehand: dagger item {label} {target} {speed}'))
        cursor = stop + .01
    if a.source_sword_hits and family in ('sword', 'shield'):
        for attack, suffix in ATTACKS:
            clip = prefix + suffix + '_out'
            event = vm.sequence_event_time(model, clip, 2102, None)
            if event is None:
                raise ValueError('Missing sword hit event: ' + clip)
            hit = marks[attack + ' max attack'] + event
            stop = marks[attack + ' min hit'] + vm.duration(model, clip)
            hit_changes[attack + ' hit'] = hit
            for strength in ('small', 'medium', 'large'):
                hit_changes[attack + ' ' + strength + ' follow start'] = hit + .001
                # A baked idle tail remains stored but is never played. The
                # ordinary idle's runtime fade owns the Source Out handoff.
                hit_changes[attack + ' ' + strength + ' follow stop'] = stop
    final_stop = float(sections[-1]['times'][-1])
    updated = [(final_stop if key == 'weapononehand: stop' else
                hit_changes.get(key.removeprefix('weapononehand: '), time), key)
               for time, key in old_marks] + added_marks
    extra.keys = np.asarray(sorted(updated, key=lambda row: row[0]), dtype='<f,O')
    preserved = append_channels(stream, vm.nif, sections)
    stream.save(path)
    yaml = path.with_suffix('.yaml')
    rules = yaml.read_text() if yaml.exists() else 'blending_rules:\n'
    for row in sections:
        key = f"weapononehand:{row['key']} hold start"
        rules += (f"  - from: '*'\n    to: '{key}'\n    easing: linear\n"
                  f"    duration: {row['fade']:.9g}\n    clock: animation\n"
                  f"  - from: ''\n    to: '{key}'\n    easing: linear\n    duration: 0\n")
    yaml.write_text(rules)
    for row in sections:
        metadata['segments'].append(dict(offset=row['offset'], duration=row['duration'], **row['source']))
        metadata.setdefault('runtime_arm_fades', []).append(dict(
            key=row['key'] + ' hold start', duration=row['fade'], clock='animation'))
    metadata['marks'] = sorted([[t, key.removeprefix('weapononehand: ')] for t, key in updated])
    if family == 'dagger':
        metadata['dagger_item_commands'] = sorted(metadata['dagger_item_commands'] + commands,
                                                key=lambda row: row['time'])
    block = path.with_name('brutal_block.kf')
    guard = vm.nif.NiStream(); guard.load(block)
    guard_keys = keys_of(guard, vm.nif)
    lower = next(float(t) for t, k in guard_keys.keys if k == 'brutal_block: lower start')
    guard_keys.keys = np.asarray(sorted([(float(t), str(k)) for t, k in guard_keys.keys]
        + [(lower + READY[family], 'brutal_block: attack ready')], key=lambda row: row[0]), dtype='<f,O')
    guard.save(block)
    relative = [path.relative_to(a.output), yaml.relative_to(a.output), block.relative_to(a.output)]
    return dict(folder=folder, old_stop=old_stop, stop=final_stop, preserved_channel_keys=preserved,
                ready_local=READY[family], source_sword_hits=hit_changes,
                sections=[{k: v for k, v in row.items() if k not in ('times', 'poses')} for row in sections],
                outputs={p.as_posix(): sha(a.output / p) for p in relative})


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--families', nargs='+', choices=tuple(FOLDERS), default=list(FOLDERS))
    p.add_argument('--source-sword-hits', action='store_true',
                   help='Use Source 2102 times and raw Out recovery ends for swords')
    a = p.parse_args()
    if a.output.exists():
        p.error('Use a fresh output directory')
    shutil.copytree(a.input, a.output)
    manifest = json.loads((a.output / 'weapon-families-import.json').read_text())
    viewmodel = json.loads((a.output / 'viewmodel-import.json').read_text())
    report = dict(converter=Path(__file__).name, input=str(a.input), families={})
    for family in a.families:
        report['families'][family] = patch_family(a, family, manifest, viewmodel)
    for info in report['families'].values():
        manifest['outputs'].update(info['outputs'])
    manifest['charged_holds'] = report
    (a.output / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
    (a.output / 'viewmodel-import.json').write_text(json.dumps(viewmodel, indent=2) + '\n')
    (a.output / 'charged-holds-import.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: dict(sections=len(v['sections']), preserved=v['preserved_channel_keys'],
                            stop=v['stop']) for k, v in report['families'].items()}, indent=2))


if __name__ == '__main__':
    main()
