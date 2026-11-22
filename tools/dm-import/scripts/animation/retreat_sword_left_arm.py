#!/usr/bin/env python3
"""One smooth retreat of the left arm for the bare sword L sections.

Measured against the rendered mesh volumes. Only existing Left Clavicle
translations change. In, hold and out share one retreat vector with a smooth
entry/exit envelope.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import shutil

import numpy as np

from retarget_kf import norm
from adapt_sword_left_contact import ActualContactRig, contacts, sample_pose, smooth
from audit_staff_mesh_grip import Rig, nif
from audit_viewmodel_continuity import tracks


def weight(label, time, start, stop):
    if label == 'in':
        return float(smooth((time - start) / .2))
    if label == 'hold':
        return 1.
    return float(smooth((stop - time) / .2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('input', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    for name in ('snapshot', 'hand-mesh'):
        parser.add_argument('--' + name, type=Path)
    parser.add_argument('--retreat-vector', type=float, nargs=3, metavar=('X', 'Y', 'Z'),
                        help='Apply this retreat vector instead of searching a rendered-geometry snapshot')
    parser.add_argument('--clearance', type=float, default=.05)
    parser.add_argument('--max-shift', type=float, default=4.)
    parser.add_argument('--audit-hz', type=int, default=120)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('The output directory must not exist yet')
    if not 0 < args.max_shift <= 4 or args.audit_hz < 120:
        parser.error('Supported range: shift <=4 units, sampling >=120Hz')
    given = args.retreat_vector is not None
    if not given and (args.snapshot is None or args.hand_mesh is None):
        parser.error('Give --retreat-vector, or both --snapshot and --hand-mesh to search for one')
    relative = Path('Animations/brutal_fp/weapononehand.kf')
    channels = tracks(args.input / relative)
    rig = Rig(args.input / 'meshes/brutal_fp/xbase_anim.1st.nif')
    actual = None if given else ActualContactRig(json.loads(args.snapshot.read_text()), args.hand_mesh)
    stream = nif.NiStream(); stream.load(args.input / relative)
    extra = next(e for e in stream.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData))
    marks = {str(key).removeprefix('weapononehand: '): float(time) for time, key in extra.keys}
    ranges = [('in', marks['thrust start'], marks['thrust max attack']),
              ('hold', marks['thrust hold start'], marks['thrust hold stop']),
              ('out', marks['thrust min hit'], marks['thrust large follow stop'])]
    samples, before = [], []
    for label, start, stop in (() if given else ranges):
        section = []
        for time in np.linspace(start, stop, max(2, int(np.ceil((stop - start) * args.audit_hz)) + 1)):
            meshes = actual.meshes(sample_pose(rig, channels, float(time)))
            values = contacts(meshes, np.zeros(3), args.clearance)
            sample = dict(label=label, time=float(time), meshes=meshes,
                          weight=weight(label, float(time), start, stop), before=values)
            samples.append(sample); section.append(values)
        before.append(dict(section=label, samples=len(section), violations=sum(not all(v['clear'] for v in row.values()) for row in section)))
        print(json.dumps(before[-1]), flush=True)
    bad = [index for index, sample in enumerate(samples) if not all(v['clear'] for v in sample['before'].values())]
    order = sorted(bad, key=lambda i: -sum(v['inside_volume_samples'] for v in samples[i]['before'].values()))
    order += [index for index in range(len(samples)) if index not in bad]
    directions = []
    centre_time = samples[bad[len(bad) // 2]]['time'] if bad else ranges[0][1]
    item_frame = sample_pose(rig, channels, centre_time)['Weapon Bone'][:3, :3]
    for basis in (np.eye(3), item_frame):
        for value in itertools.product((-1., 0., 1.), repeat=3):
            vector = basis @ np.asarray(value)
            if norm(vector) < .1:
                continue
            vector /= norm(vector)
            if not any(norm(vector - old) < 1e-6 for old in directions):
                directions.append(vector)
    def valid(vector):
        for index in order:
            sample = samples[index]
            values = contacts(sample['meshes'], vector * sample['weight'], args.clearance)
            if not all(value['clear'] for value in values.values()):
                return False
        return True
    found = np.asarray(args.retreat_vector) if given else np.zeros(3) if not bad else None
    for amount in np.arange(.1, args.max_shift + .00001, .1):
        if found is not None:
            break
        for direction in directions:
            if valid(direction * amount):
                found = direction * amount
                break
        print(json.dumps(dict(searched_radius=float(amount), feasible=found is not None)), flush=True)
    report = dict(status='applied', input=str(args.input), before=before,
        scope='Only bare Sword _B Left Clavicle translations; all local rotations/other node keys unchanged',
        volume='Actual rendered triangles; only manifold boundary rings closed by oriented fan; no hull/capsule',
        audit_hz=args.audit_hz, clearance=args.clearance)
    if given:
        report.update(volume='Given retreat vector applied; no rendered-geometry search')
    else:
        report.update(snapshot=str(args.snapshot), snapshot_sha256=hashlib.sha256(args.snapshot.read_bytes()).hexdigest(),
                      hand_mesh=str(args.hand_mesh))
    if found is None:
        report['status'] = 'no retreat within bound'
        args.output.parent.mkdir(parents=True, exist_ok=True)
        (args.output.parent / 'retreat-unsolved.json').write_text(json.dumps(report, indent=2) + '\n')
        raise SystemExit('No coherent retreat within bound; no KF written')
    if not given and norm(found) > .001:
        direction = found / norm(found)
        low, high = max(0., norm(found) - .1), norm(found)
        for _ in range(12):
            middle = (low + high) * .5
            if valid(direction * middle):
                high = middle
            else:
                low = middle
        found = direction * (high + .01)
    shutil.copytree(args.input, args.output)
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = dict(zip(names, stream.root.controllers))
    data = controllers['Bip01 L Clavicle'].data.translations
    original = data.keys.copy(); after = original.copy(); changed = 0
    for index, row in enumerate(after):
        time = float(row[0])
        own = next(((label, start, stop) for label, start, stop in ranges if start - 1e-5 <= time <= stop + 1e-5), None)
        if own is None:
            continue
        label, start, stop = own
        delta = found * weight(label, time, start, stop)
        parent = sample_pose(rig, channels, time)[rig.parents['Bip01 L Clavicle']]
        after[index, 1:] += parent[:3, :3].T @ delta
        changed += norm(delta) > 1e-10
    data.keys = after
    stream.save(args.output / relative)
    output = tracks(args.output / relative)
    failures = []
    for sample in samples:
        values = contacts(actual.meshes(sample_pose(rig, output, sample['time'])), np.zeros(3), args.clearance)
        if not all(value['clear'] for value in values.values()):
            failures.append(dict(section=sample['label'], time=sample['time'], parts=values))
    for name in channels:
        if channels[name][0].tobytes() != output[name][0].tobytes():
            raise ValueError('Local joint rotation changed: ' + name)
        if name != 'Bip01 L Clavicle' and channels[name][1].tobytes() != output[name][1].tobytes():
            raise ValueError('Unrelated translation changed: ' + name)
    report.update(retreat_vector=found.tolist(), maximum_shift=float(norm(found)),
                  changed_translation_keys=int(changed), after_dense_failures=failures,
                  sha256=hashlib.sha256((args.output / relative).read_bytes()).hexdigest())
    manifest_path = args.output / 'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text()); manifest['outputs'][relative.as_posix()] = report['sha256']
    manifest['sword_left_retreat'] = report
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    (args.output / 'sword-left-retreat.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: report[key] for key in ('status', 'retreat_vector', 'maximum_shift', 'after_dense_failures')}, indent=2))
    if failures:
        raise SystemExit('Serialized retreat interpolation still contacts actual volume')


if __name__ == '__main__':
    main()
