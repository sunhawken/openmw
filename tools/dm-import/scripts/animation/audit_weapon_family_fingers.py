#!/usr/bin/env python3
"""Compare emitted staff/dagger finger directions with Dark Messiah's clips.

Reads the KF output, not the retargeter's poses. All ten fingers and all three
phalanges are required, including the inconsistently named right thumb/little
finger. Source fades are excluded. Knuckle positions and phalanx lengths must
remain native. With --baseline, also checks that arms and weapon mounts have not
moved and reports that set's finger error against the same source clocks.
"""
import argparse
import json
from pathlib import Path

import numpy as np

from retarget_kf import norm
from audit_viewmodel_continuity import tracks
from import_viewmodel import BASIS, FPS
from import_weapon_families import Family
from retarget_kf import quat_matrix


def angle(first, second):
    return float(np.degrees(np.arccos(np.clip(
        first @ second / norm(first) / norm(second), -1., 1.))))


def source_chains():
    # Use the actual MDL spelling here, independently of the converter's lookup.
    for side in 'LR':
        for index, letter in enumerate(('Thumb_', 'Finger_A', 'Finger_B', 'Finger_C', 'Finger_D')):
            names = [f'SK_{side}_{letter}{i}' for i in range(1, 5)]
            if side == 'R':
                if index == 0:
                    names = ['Sk_R_Thumb_1', 'SK_R_Thumb_2', 'Sk_R_Thumb_3', 'Sk_R_Thumb_4']
                elif index == 1:
                    names[3] = 'Sk_R_Finger_A4'
                elif index == 4:
                    names = [f'SK_Finger_D{i}' for i in range(1, 5)]
            yield side, index, names


def pose_at(channels, index):
    return {name: (quat_matrix(rotations[index, 1:][[1, 2, 3, 0]]), positions[index, 1:])
            for name, (rotations, positions) in channels.items()}


def grip_state(vm, side, clip, time):
    rotation = vm.dagger_state(side, clip, time)
    for state in ('forward', 'reverse'):
        if np.allclose(rotation, vm.dagger_grip(state), atol=1e-5):
            return state
    return 'turning'


def undo_declared_staff_fit(pose, dm, vm, declaration):
    """Remove only declared native anatomy offsets from emitted local KF tracks.

    The remaining articulation must still follow the actual source at the same
    strict tolerance. This is separate from the mesh contact/penetration check.
    """
    corrected = dict(pose)
    fit = declaration['staff_grip_fit']
    axes = declaration['staff_grip_fit_axes']
    idle_curls = np.asarray(declaration['staff_idle_curls'])
    source = {name.casefold(): value for name, value in dm.items()}
    origin, direction = vm.shaft(dm)
    offset = dm['SK_L_Hand'][:3, 3]-origin
    distance = norm(offset-direction*np.dot(offset, direction))
    ramp = np.clip((distance-vm.dm_grip_distance-1.)/3., 0., 1.)
    palm = 1.-ramp*ramp*(3.-2.*ramp)
    weights = []
    for i, letter in enumerate(('thumb_', 'finger_a', 'finger_b', 'finger_c', 'finger_d')):
        points = [source['sk_l_'+letter+str(k)][:3, 3] for k in (1, 2, 3, 4)]
        vectors = np.diff(points, axis=0)
        vectors /= norm(vectors, axis=1)[:, None]
        curl = np.arccos(np.clip(np.sum(vectors[:-1]*vectors[1:], axis=1), -1., 1.)).sum()
        closure = np.clip(curl/idle_curls[i], 0., 1.)
        weight = palm*closure*closure*(3.-2.*closure)
        weights.append(float(weight))
        for k, suffix in enumerate(('', '1', '2')):
            axis = np.asarray(axes[i][k]); theta = -fit['finger_bends'][i][k]*weight
            skew = np.array([[0., -axis[2], axis[1]], [axis[2], 0., -axis[0]],
                             [-axis[1], axis[0], 0.]])
            undo = np.eye(3)+np.sin(theta)*skew+(1.-np.cos(theta))*skew@skew
            name = f'Bip01 L Finger{i}{suffix}'
            rotation, translation = pose[name]
            corrected[name] = rotation@undo, translation
    rotation, translation = corrected['Bip01 L Finger0']
    corrected['Bip01 L Finger0'] = rotation, translation-np.asarray(fit['thumb_root_delta'])*weights[0]
    return corrected, weights


def inspect(data, vm, groups, declaration=None):
    result = dict(data=str(data), samples=0, phalanges=0, max_degrees=0.,
                  max_native_translation_error=0., max_raw_source_degrees=0.,
                  declared_native_fit=bool(declaration), fit_zero_samples=0,
                  fingers={}, clips=[], grips={}, errors=[])
    seen = set()
    for folder, clips in groups.items():
        for group, info in clips.items():
            channels = tracks(data/'Animations'/folder/(group+'.kf'))
            times = channels['Camera'][0][:, 0]
            for segment in info['segments']:
                natural, fade = segment['natural'], segment['fade_in']
                raw_duration = segment['duration']-segment['return_fade']
                raw_indices = np.flatnonzero((times >= segment['offset']-1e-5)
                    & (times <= segment['offset']+raw_duration+1e-5))
                if segment['loop']:
                    original_duration = ((segment.get('loop_period') or raw_duration)
                        + segment.get('loop_lead_in', 0.))
                else:
                    # Explicit non-loop durations can exceed the MDL's natural
                    # length (the clinch loss has its own head-animation clock).
                    # Use the actual emitted raw sampling span, rather than
                    # rescaling those keys to max(natural, entry fade).
                    emitted_span = float(times[raw_indices[-1]]) - segment['offset']
                    original_duration = (raw_duration if abs(emitted_span - raw_duration) < 1e-5
                                         else emitted_span)
                for index, time in enumerate(times):
                    local = float(time)-segment['offset']
                    if local < fade-1e-5 or local > raw_duration+1e-5:
                        continue
                    # KF stores its times as float32. Recover the converter's
                    # original sampling clock before evaluating a moving fit:
                    # sub-microsecond timestamp rounding otherwise changes the
                    # predicted thumb translation near a fast opening.
                    steps = max(1, len(raw_indices)-1)
                    local = (index-int(raw_indices[0]))*original_duration/steps
                    period = segment.get('loop_period') or raw_duration
                    source_time = ((np.clip(local, 0., raw_duration) % period)*natural/period
                                   if segment.get('loop_lead_in', 0.) else
                                   np.clip(local, 0., raw_duration)*natural/raw_duration
                                   ) if segment['loop'] else np.clip(local, 0., natural)
                    dm = vm.dm_world(segment['model'], segment['clip'], source_time)
                    pose = pose_at(channels, index)
                    raw_world = vm.fk(pose)
                    if declaration:
                        pose, weights = undo_declared_staff_fit(pose, dm, vm, declaration)
                        result['fit_zero_samples'] += int(max(weights) < 1e-6)
                    world = vm.fk(pose)
                    result['samples'] += 1
                    seen.add(segment['clip'])
                    if vm.family == 'dagger':
                        for side in 'LR':
                            item_time = max(0., local) if segment['loop'] else source_time
                            label = side+' '+grip_state(vm, side, segment['clip'], item_time)
                            result['grips'][label] = result['grips'].get(label, 0)+1
                    for side, finger, source_names in source_chains():
                        points = [dm[name][:3, 3] for name in source_names]
                        for k, suffix in enumerate(('', '1', '2')):
                            name = f'Bip01 {side} Finger{finger}{suffix}'
                            # Native phalanges extend on +X; the source's fourth joint is the tip.
                            error = angle(world[name][:3, 0], BASIS@(points[k+1]-points[k]))
                            raw_error = angle(raw_world[name][:3, 0], BASIS@(points[k+1]-points[k]))
                            result['max_raw_source_degrees'] = max(result['max_raw_source_degrees'], raw_error)
                            translation_error = float(np.max(np.abs(pose[name][1]-vm.local(name)[1])))
                            result['max_native_translation_error'] = max(
                                result['max_native_translation_error'], translation_error)
                            label = side+str(finger)+suffix
                            if error > result['fingers'].get(label, {}).get('max_degrees', -1.):
                                result['fingers'][label] = dict(max_degrees=error, group=group,
                                    clip=segment['clip'], time=float(source_time))
                            result['max_degrees'] = max(result['max_degrees'], error)
                            result['phalanges'] += 1
    result['clips'] = sorted(seen)
    if not result['samples'] or len(result['fingers']) != 30:
        result['errors'].append('Missing samples or a finger channel')
    if result['max_degrees'] > .1:
        result['errors'].append('Source finger direction differs by more than .1 degree')
    if result['max_native_translation_error'] > 1e-5:
        result['errors'].append('Native knuckle positions or phalanx lengths changed')
    if vm.family == 'dagger' and len(result['grips']) != 6:
        result['errors'].append('Both hands must be checked in forward, reverse and turning grips')
    return result


def unchanged_except_fingers(data, baseline, groups):
    maximum, count = 0., 0
    for folder, clips in groups.items():
        for group in clips:
            relative = Path('Animations')/folder/(group+'.kf')
            before, after = tracks(baseline/relative), tracks(data/relative)
            if before.keys() != after.keys():
                raise ValueError(f'{relative}: bone channels changed')
            for name, (rotations, positions) in after.items():
                old_r, old_p = before[name]
                if rotations.shape != old_r.shape or positions.shape != old_p.shape:
                    raise ValueError(f'{relative}: source clock changed')
                if not np.array_equal(rotations[:, 0], old_r[:, 0]):
                    raise ValueError(f'{relative}: key times changed')
                maximum = max(maximum, float(np.max(np.abs(positions-old_p))))
                if 'Finger' not in name:
                    # Quaternion signs may be arbitrary; compare rotations up to sign.
                    a, b = rotations[:, 1:], old_r[:, 1:]
                    maximum = max(maximum, float(np.max(np.minimum(
                        np.max(np.abs(a-b), axis=1), np.max(np.abs(a+b), axis=1)))))
                count += len(rotations)
    return dict(keys=count, max_change=maximum,
                errors=['Non-finger motion changed'] if maximum > 1e-5 else [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path)
    for name in ('vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--family', choices=('staff', 'dagger', 'all'), default='all')
    parser.add_argument('--allow-motion-changes', action='store_true',
                        help='Do not require unchanged clocks/arms when also changing source events or transitions')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.data/'weapon-families-import.json').read_text())
    report = {}
    for family in ('staff', 'dagger'):
        if family not in manifest or args.family not in ('all', family):
            continue
        vm = Family(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                    camera_contract=args.camera_contract, native_eye_height=True)
        vm.grip_path = args.grip_animation
        vm.configure(family)
        groups = manifest[family]['groups']
        declaration = manifest[family] if 'staff_grip_fit' in manifest[family] else None
        report[family] = dict(result=inspect(args.data, vm, groups, declaration))
        if args.baseline:
            report[family]['baseline'] = inspect(args.baseline, vm, groups)
            if not args.allow_motion_changes:
                report[family]['preserved_motion'] = unchanged_except_fingers(args.data, args.baseline, groups)
    text = json.dumps(report, indent=2)+'\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')
    raise SystemExit(not report or any(v['result']['errors'] or v.get('preserved_motion', {}).get('errors')
                                      for v in report.values()))


if __name__ == '__main__':
    main()
