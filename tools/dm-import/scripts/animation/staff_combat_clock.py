#!/usr/bin/env python3
"""Update only the staff strike clocks of a first-person data directory.

The source Combat Staff uses speed_attack=1.10 for fast A-D and charged outs.
Keep the established section starts, windups, guards, source regrips, and other
weapon families. Remove the additional baked charged-out idle tail: the engine's
normal idle crossfade owns that transition.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np

from import_weapon_families import Family
from retarget_kf import matrix_quat


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--vpk-root', type=Path, required=True)
    p.add_argument('--work-dir', type=Path, required=True)
    p.add_argument('--skeleton', type=Path, required=True)
    p.add_argument('--grip-animation', type=Path, required=True)
    p.add_argument('--camera-contract', type=Path, required=True)
    a = p.parse_args()
    if a.output.exists():
        p.error('The output directory must not exist yet')
    manifest = json.loads((a.input/'weapon-families-import.json').read_text())
    vm = Family(a.vpk_root, a.work_dir, a.skeleton, a.grip_animation,
                -1., None, True, a.camera_contract, True)
    vm.grip_path = a.grip_animation
    vm.staff_mount_offset = float(manifest['staff'].get('staff_mount_offset', 0.))
    vm.configure('staff')
    relative = Path('Animations/brutal_fp_staff/weapononehand.kf')
    stream = vm.nif.NiStream(); stream.load(a.input/relative)
    extra = next(e for e in stream.root.extra_datas if isinstance(e, vm.nif.NiTextKeyExtraData))
    old_marks = [(float(time), str(label)) for time, label in extra.keys]
    marks = dict((label.removeprefix('weapononehand: '), time) for time, label in old_marks)
    updates = {}
    sections = []
    armed = vm.stance_pose()
    model = vm.config['model']
    for attack, suffix in (('chop', '_T'), ('slash', '_L'), ('thrust', '_B'), ('neutral', '')):
        prefix = vm.config['attack']+suffix
        roll = {}
        _, windup, _ = vm.segment(model, prefix+'_in', roll_state=roll, start_pose=armed[0], runtime_entry=True)
        vm.pending.pop()
        times, poses, error = vm.segment(model, prefix+'_out', roll_state=roll, start_pose=windup[-1])
        source = dict(vm.pending.pop())
        start = marks[attack+' min hit']
        max_attack = marks[attack+' max attack']
        stop = start+float(times[-1])
        hit = max_attack+vm.strike_time(prefix+'_out', suffix, float(times[-1]))
        updates[attack+' hit'] = hit
        for strength in ('small', 'medium', 'large'):
            updates[attack+' '+strength+' follow start'] = hit+.001
            updates[attack+' '+strength+' follow stop'] = stop
        if attack+' chain' in marks:
            updates[attack+' chain'] = start+vm.config['chain'][suffix]/vm.source_rate(model, prefix+'_out')
        events = vm.sequence_events(model, prefix+'_out')
        second = next(time for event, time in events if event == 2139)
        swing = max(time for event, time in events if event == 2107 and time < second)
        updates[attack+' swing2'] = max_attack+swing
        updates[attack+' hit2'] = max_attack+second
        sections.append(dict(key=attack, clip=prefix+'_out', start=start,
            old_stop=marks[attack+' large follow stop'], stop=stop,
            times=times+start, poses=poses, source=source, max_error=error,
            source_events=[dict(event=event, real_time=time,
                                source_time=time*vm.source_rate(model, prefix+'_out')) for event, time in events]))
    for letter in 'ABCD':
        key = 'fast'+letter.lower()
        clip = 'anm_fastattack_staff_'+letter
        times, poses, error = vm.segment(model, clip, roll_state={}, start_pose=armed[0], runtime_entry=True)
        source = dict(vm.pending.pop())
        start = marks[key+' start']
        old_stop = marks[key+' stop']
        for label in marks:
            if label.startswith(key+' ') and label != key+' start':
                updates[label] = start+(marks[label]-start)/vm.source_rate(model, clip)
        updates[key+' stop'] = start+float(times[-1])
        sections.append(dict(key=key, clip=clip, start=start, old_stop=old_stop,
            stop=start+float(times[-1]), times=times+start, poses=poses, source=source, max_error=error,
            source_events=[dict(event=event, real_time=time,
                                source_time=time*vm.source_rate(model, clip)) for event, time in vm.sequence_events(model, clip)]))
    # Keep the poses after the entry fade, rather than
    # resampling a new 30 Hz grid and slightly changing the wrist trajectory.
    # Only the entry fade needs fresh samples because its duration stays .2 s.
    reference_times = list(stream.root.controllers)[0].data.rotations.keys[:, 0]
    for section in sections:
        start, rate = section['start'], section['source']['source_rate']
        raw_duration = section['source']['source_natural']
        raw_indices = np.flatnonzero((reference_times >= start-1e-5)
                                    & (reference_times <= start+raw_duration+1e-5))
        raw_clocks = np.linspace(0., raw_duration, len(raw_indices))
        retained = raw_clocks/rate >= .2-1e-9
        entry = np.flatnonzero(section['times']-start < .2-1e-9)
        entry_clocks = section['times'][entry]-start
        section.update(entry_indices=entry, baseline_indices=raw_indices[retained],
                       times=np.concatenate([entry_clocks, raw_clocks[retained]/rate])+start,
                       source_sample_clocks=np.concatenate([entry_clocks*rate, raw_clocks[retained]]).tolist(),
                       entry_samples=len(entry), kept_pose_samples=int(retained.sum()),
                       hit_timing=('First hit follows the existing forward-shaft contact adaptation; '
                                   'other hit marks follow raw source events' if section['key'] == 'slash'
                                   else 'Raw source events / source_rate'))
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, vm.nif.NiStringExtraData)]
    preserved, kept = 0, 0
    for name, controller in zip(names, stream.root.controllers):
        for field in ('rotations', 'translations'):
            data = getattr(controller.data, field)
            before = data.keys.copy()
            keep = np.ones(len(before), dtype=bool)
            added = []
            for section in sections:
                keep &= (before[:, 0] < section['start']-1e-5) | (before[:, 0] > section['old_stop']+1e-5)
                values = [matrix_quat(section['poses'][i][name][0]) if field == 'rotations'
                          else section['poses'][i][name][1] for i in section['entry_indices']]
                values = np.asarray(values)
                if field == 'rotations':
                    # Match the writer's continuous quaternion hemisphere. A
                    # matrix conversion may otherwise flip sign mid-regrip.
                    reference = before[np.argmin(abs(before[:, 0]-section['start'])), 1:]
                    if np.dot(reference, values[0]) < 0:
                        values[0] *= -1
                    for i in range(1, len(values)):
                        if np.dot(values[i-1], values[i]) < 0:
                            values[i] *= -1
                    # Keep every original quaternion value after the fade,
                    # choosing only the new entry samples' common hemisphere.
                    if np.dot(values[-1], before[section['baseline_indices'][0], 1:]) < 0:
                        values *= -1
                old_values = before[section['baseline_indices'], 1:]
                values = np.concatenate([values, old_values])
                kept += len(old_values)
                added.append(np.column_stack([section['times'], values]).astype(before.dtype))
            combined = np.concatenate([before[keep]]+added)
            data.keys = combined[np.argsort(combined[:, 0], kind='stable')]
            mask = np.ones(len(data.keys), dtype=bool)
            for section in sections:
                mask &= (data.keys[:, 0] < section['start']-1e-5) | (data.keys[:, 0] > section['old_stop']+1e-5)
            if before[keep].tobytes() != data.keys[mask].tobytes():
                raise ValueError('Preserved channel changed: '+name+':'+field)
            preserved += int(keep.sum())
    new_marks = [(updates.get(label.removeprefix('weapononehand: '), time), label) for time, label in old_marks]
    extra.keys = np.array(sorted(new_marks, key=lambda row: row[0]), dtype='<f,O')
    untouched_before = [(time, label) for time, label in old_marks
                        if label.removeprefix('weapononehand: ') not in updates]
    untouched_after = [(float(time), str(label)) for time, label in extra.keys
                       if str(label).removeprefix('weapononehand: ') not in updates]
    if untouched_before != untouched_after:
        raise ValueError('Unrelated text keys changed')
    shutil.copytree(a.input, a.output)
    stream.save(a.output/relative)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = dict(input=str(a.input.resolve()), output=str(a.output.resolve()),
        source_profile='staff_combat.qct speed_attack=1.10',
        real_arm_entry_fade=.2, native_playback_rate=1., preserved_channel_keys=preserved,
        kept_pose_channel_values_identical=kept,
        unchanged_text_keys=len(untouched_before), source_sha256=sha(a.input/relative),
        output_sha256=sha(a.output/relative),
        sections=[{k: v for k, v in section.items() if k not in ('times', 'poses', 'entry_indices', 'baseline_indices')}
                  for section in sections],
        changed_marks=[dict(label=label.removeprefix('weapononehand: '), before=time,
                            after=updates[label.removeprefix('weapononehand: ')]) for time, label in old_marks
                       if label.removeprefix('weapononehand: ') in updates],
        recipe_sha256=sha(Path(__file__)))
    (a.output/'staff-combat-clock-import.json').write_text(json.dumps(report, indent=2)+'\n')
    manifest['staff']['combat_clock_manifest'] = 'staff-combat-clock-import.json'
    manifest['outputs'][relative.as_posix()] = report['output_sha256']
    (a.output/'weapon-families-import.json').write_text(json.dumps(manifest, indent=1)+'\n')
    print(json.dumps(dict(output=str(a.output), sections=len(sections), preserved_channel_keys=preserved), indent=2))


if __name__ == '__main__':
    main()
