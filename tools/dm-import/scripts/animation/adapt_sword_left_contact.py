#!/usr/bin/env python3
"""Adapt the bare sword _B left arm to the native hand mesh.

Only existing Clavicle translation keys of the bare sword _B sections change.
Local joint and finger rotations, weapon keys, clocks and event labels stay as
they are. Contact is measured against the female hand mesh from the user's game.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import shutil

import numpy as np

from audit_dagger_native_grip import Hand
from audit_staff_mesh_grip import Rig, nif, sample_channels
from audit_viewmodel_continuity import tracks
from sword_anatomy_arms import world
from retarget_kf import quat_matrix
from sword_contact_geometry import contact, close_open_boundaries, interior_points


def matrix(bone):
    result = np.eye(4)
    result[:3, :3] = np.column_stack([bone[axis] for axis in 'xyz'])
    result[:3, 3] = bone['position']
    return result


def transform(triangles, frame):
    return triangles @ frame[:3, :3].T + frame[:3, 3]


class ActualContactRig:
    def __init__(self, snapshot, hand_mesh):
        geometry, bones = snapshot['geometry'], snapshot['body']['weaponCycleBones']
        if Path(geometry['models']['leftHand']).name.casefold() != hand_mesh.name.casefold():
            raise ValueError('Hand mesh does not match the actually rendered part')
        self.hand = Hand(nif, hand_mesh, 'L')
        self.parts = {}
        for part, native, observed in [('leftForearm', 'Bip01 L Forearm', 'leftForearm'),
                                        ('leftUpperArm', 'Bip01 L UpperArm', 'leftUpperArm'),
                                        ('rightItem', 'Weapon Bone', 'weapon')]:
            root = matrix(geometry['roots'][part])
            self.parts[part] = (native, np.linalg.inv(matrix(bones[observed])) @ root,
                transform(np.asarray(geometry['triangles'][part]).reshape(-1, 3, 3), np.linalg.inv(root)))

    def meshes(self, pose):
        result = {'leftHand': self.hand.triangles(pose)}
        for part, (bone, offset, triangles) in self.parts.items():
            result[part] = transform(triangles, pose[bone] @ offset)
        for part in ('leftHand', 'leftForearm', 'leftUpperArm'):
            result[part + 'Volume'], result[part + 'BoundaryRings'] = close_open_boundaries(result[part])
        result['rightItemPoints'] = np.concatenate((result['rightItem'].reshape(-1, 3), result['rightItem'].mean(1)))
        return result


def smooth(value):
    value = np.clip(value, 0., 1.)
    return value * value * (3 - 2 * value)


def envelope(time, knots):
    first, full, leave, last = knots
    if time <= first or time >= last:
        return 0.
    if time < full:
        return float(smooth((time - first) / (full - first)))
    if time > leave:
        return float(smooth((last - time) / (last - leave)))
    return 1.


def sample_pose(rig, channels, time):
    pose = {name: (quat_matrix(q[[1, 2, 3, 0]]), position)
            for name, (q, position) in sample_channels(channels, time).items()}
    return world(rig, pose)


def contacts(meshes, delta, clearance):
    result = {}
    for part in ('leftHand', 'leftForearm', 'leftUpperArm'):
        value = contact(meshes['rightItem'], meshes[part + 'Volume'] + delta, clearance)
        # A whole disconnected guard can enter through a wrist opening and sit
        # inside the arm without crossing its lateral rendered faces. Explicit
        # boundary closure plus containment is required; surface pairs alone
        # cannot detect penetration.
        contained = interior_points(meshes[part + 'Volume'] + delta, meshes['rightItemPoints'])
        value['inside_volume_samples'] = int(contained.sum())
        value['clear'] = value['clear'] and not contained.any()
        result[part] = value
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for argument in ('input', 'output', 'snapshot', 'hand-mesh'):
        parser.add_argument('--' + argument, required=True, type=Path)
    parser.add_argument('--clearance', type=float, default=.05)
    parser.add_argument('--max-shift', type=float, default=2.)
    parser.add_argument('--audit-hz', type=int, default=120)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    if not 0 < args.max_shift <= 2 or args.audit_hz < 120:
        parser.error('Controlled scope requires shift <=2 native units and audit >=120Hz')
    snapshot = json.loads(args.snapshot.read_text())
    actual = ActualContactRig(snapshot, args.hand_mesh)
    relative = Path('Animations/brutal_fp/weapononehand.kf')
    channels = tracks(args.input / relative)
    rig = Rig(args.input / 'meshes/brutal_fp/xbase_anim.1st.nif')
    scene = nif.NiStream(); scene.load(args.input / relative)
    extra = next(e for e in scene.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))
    marks = {str(key).removeprefix('weapononehand: '): float(time) for time, key in extra.keys}
    ranges = [('in', marks['thrust start'], marks['thrust max attack']),
              ('out', marks['thrust min hit'], marks['thrust large follow stop']),
              ('hold', marks['thrust hold start'], marks['thrust hold stop'])]
    report = dict(status='applied', input=str(args.input),
        snapshot=str(args.snapshot), snapshot_sha256=hashlib.sha256(args.snapshot.read_bytes()).hexdigest(),
        hand_mesh=str(args.hand_mesh), hand_mesh_sha256=hashlib.sha256(args.hand_mesh.read_bytes()).hexdigest(),
        clearance=args.clearance, audit_hz=args.audit_hz, parts=snapshot['geometry']['models'],
        scope='Bare Sword _B only; coherent whole-left-arm translation, no local joint rotation changes', sections=[])
    corrections = []
    for label, start, stop in ranges:
        times = np.linspace(start, stop, max(2, int(np.ceil((stop - start) * args.audit_hz)) + 1))
        samples = [(float(time), actual.meshes(sample_pose(rig, channels, float(time)))) for time in times]
        raw = [contacts(meshes, np.zeros(3), args.clearance) for time, meshes in samples]
        bad = [index for index, row in enumerate(raw) if not all(part['clear'] for part in row.values())]
        section = dict(section=label, start=start, stop=stop, samples=len(samples),
            before_contact_samples=len(bad), before=raw)
        report['sections'].append(section)
        print(json.dumps(dict(section=label, samples=len(samples), contact_samples=len(bad))), flush=True)
        if not bad:
            continue
        first, last = samples[bad[0]][0], samples[bad[-1]][0]
        if first <= start + .03 or last >= stop - .03:
            raise ValueError(f'{label}: contact reaches a section endpoint; no coherent phase handoff')
        knots = (max(start, first - .15), max(start + .01, first - 1 / 30),
                 min(stop - .01, last + 1 / 30), min(stop, last + .15))
        weights = [envelope(time, knots) for time, meshes in samples]
        order = bad + [index for index in range(len(samples)) if index not in bad]
        centre = samples[bad[len(bad) // 2]][1]['rightItem'].mean((0, 1))
        frame = sample_pose(rig, channels, samples[bad[len(bad) // 2]][0])['Weapon Bone'][:3, :3]
        directions = []
        for basis in (np.eye(3), frame):
            for value in itertools.product((-1., 0., 1.), repeat=3):
                direction = basis @ np.asarray(value)
                if np.linalg.norm(direction) < .1:
                    continue
                direction /= np.linalg.norm(direction)
                if not any(np.linalg.norm(direction - existing) < 1e-6 for existing in directions):
                    directions.append(direction)
        weapon_frames = [sample_pose(rig, channels, time)['Weapon Bone'][:3, :3] for time, meshes in samples]
        def valid(vector, space):
            for index in order:
                delta = vector if space == 'world' else weapon_frames[index] @ vector
                if not all(part['clear'] for part in contacts(samples[index][1], delta * weights[index], args.clearance).values()):
                    return False
            return True
        found = None
        for length in np.arange(.05, args.max_shift + .0001, .05):
            for space in ('world', 'weapon'):
                for direction in directions:
                    local_direction = direction if space == 'world' else frame.T @ direction
                    if valid(local_direction * length, space):
                        found = local_direction * length
                        found_space = space
                        break
                if found is not None:
                    break
            if found is not None:
                break
        if found is None:
            report['status'] = 'no feasible shift within 2 units'
            args.output.parent.mkdir(parents=True, exist_ok=True)
            (args.output.parent / 'left-contact-unsolved.json').write_text(json.dumps(report, indent=2) + '\n')
            raise ValueError(f'{label}: no coherent whole-arm correction within {args.max_shift} units')
        direction = found / np.linalg.norm(found)
        low, high = max(0., np.linalg.norm(found) - .05), np.linalg.norm(found)
        for _ in range(12):
            middle = (low + high) * .5
            if valid(direction * middle, found_space):
                high = middle
            else:
                low = middle
        vector = direction * (high + .002)
        corrections.append((start, stop, knots, vector, found_space))
        section.update(envelope_knots=list(knots), translation_vector=vector.tolist(), translation_space=found_space,
            maximum_shift=float(np.linalg.norm(vector)),
            search='First feasible .05-unit radius over deterministic native-view/weapon-frame directions, then radial bisection')
        print(json.dumps(dict(section=label, shift=vector.tolist(), norm=float(np.linalg.norm(vector)))), flush=True)
    shutil.copytree(args.input, args.output)
    names = [e.string_data for e in scene.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = dict(zip(names, scene.root.controllers))
    controller = controllers['Bip01 L Clavicle']
    original = controller.data.translations.keys.copy()
    after = original.copy()
    changed = []
    for index, row in enumerate(after):
        time = float(row[0])
        pose = sample_pose(rig, channels, time)
        delta = sum(((vector if space == 'world' else pose['Weapon Bone'][:3, :3] @ vector) * envelope(time, knots)
                     for start, stop, knots, vector, space in corrections
                     if start - 1e-5 <= time <= stop + 1e-5), np.zeros(3))
        if np.linalg.norm(delta) < 1e-10:
            continue
        parent = pose[rig.parents['Bip01 L Clavicle']]
        after[index, 1:] += parent[:3, :3].T @ delta
        changed.append(index)
    controller.data.translations.keys = after
    scene.save(args.output / relative)
    updated = tracks(args.output / relative)
    failures = []
    for label, start, stop in ranges:
        for time in np.linspace(start, stop, max(2, int(np.ceil((stop - start) * args.audit_hz)) + 1)):
            values = contacts(actual.meshes(sample_pose(rig, updated, float(time))), np.zeros(3), args.clearance)
            if not all(part['clear'] for part in values.values()):
                failures.append(dict(section=label, time=float(time), parts=values))
    for name in channels:
        before_q, before_t = channels[name]
        after_q, after_t = updated[name]
        if before_q.tobytes() != after_q.tobytes() or (name != 'Bip01 L Clavicle' and before_t.tobytes() != after_t.tobytes()):
            raise ValueError('Unrelated channel keys changed: ' + name)
    report.update(changed_translation_keys=len(changed), after_dense_failures=failures,
        maximum_shift=max((float(np.linalg.norm(row[3])) for row in corrections), default=0.),
        sha256=hashlib.sha256((args.output / relative).read_bytes()).hexdigest())
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text())
    manifest['outputs'][relative.as_posix()] = report['sha256']
    manifest['sword_left_contact'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'sword-left-contact.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('status', 'maximum_shift', 'changed_translation_keys', 'after_dense_failures')}, indent=2))
    if failures:
        raise SystemExit('Serialized interpolation still has contact')


if __name__ == '__main__':
    main()
