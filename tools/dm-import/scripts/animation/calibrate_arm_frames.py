#!/usr/bin/env python3
"""Apply a constant REST arm-frame calibration to generic first-person KFs.

For each side:  U' = U*Cu,  F' = Cu^T*F*Cf,  H' = Cf^T*H  (local rotation keys of
Bip01 {R,L} UpperArm / Forearm / Hand). Cu and Cf are proper rotations about the
child translation axes, so translations stay unchanged and world Hand and all of its
descendants (fingers, Weapon/Shield Bone, left weapon, dagger frames) are preserved at
keys, between keys and in blends of equally calibrated poses. Upper/Forearm skin pose
changes intentionally; skeleton binds are NOT touched.

It is a constant calibration, not a dynamic rest-relative solve.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

import numpy as np

from retarget_kf import norm
HERE = Path(__file__).resolve()
sys.path.insert(0, str(HERE.parent))
sys.path.insert(0, str(HERE.parents[2] / 'third_party/io_scene_mw/lib'))
from es3 import nif  # noqa: E402
from retarget_kf import matrix_quat, quat_matrix  # noqa: E402

FOLDERS = ('brutal_fp', 'brutal_fp_shield', 'brutal_fp_staff', 'brutal_fp_dagger')
REPORT = 'arm-frame-calibration-import.json'
MARKER = 'constant_arm_frame_calibration'
# Metadata written by the sword _B anatomy step (manifest key and its report fields).
ANATOMY_MARKERS = ('"sword_anatomy"', '"rest_relative_wrist"', '"native_rest_hinge"')
TOLERANCE = 1e-5


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def chain(side):
    return [f'Bip01 {side} UpperArm', f'Bip01 {side} Forearm', f'Bip01 {side} Hand']


def load_calibration(path, skeleton):
    raw = json.loads(path.read_text())
    stream = nif.NiStream(); stream.load(skeleton)
    nodes = {n.name: n for n in stream.objects_of_type(nif.NiNode) if n.name}
    parents = {c.name: n.name for n in nodes.values() for c in n.children if isinstance(c, nif.NiNode) and c.name}
    result = {}
    for side in ('R', 'L'):
        if side not in raw or set(raw[side]) != {'Cu', 'Cf'}:
            raise ValueError(f'Calibration needs exactly {side}.Cu and {side}.Cf')
        upper, fore, hand = chain(side)
        if parents.get(fore) != upper or parents.get(hand) != fore:
            raise ValueError(f'Unexpected skeleton parents for {side} arm chain')
        mats = {}
        for key, child in (('Cu', fore), ('Cf', hand)):
            m = np.asarray(raw[side][key], float)
            if m.shape != (3, 3) or not np.isfinite(m).all():
                raise ValueError(f'{side}.{key} is not a finite 3x3 matrix')
            if np.abs(m @ m.T - np.eye(3)).max() > TOLERANCE or abs(np.linalg.det(m) - 1) > TOLERANCE:
                raise ValueError(f'{side}.{key} is not a proper rotation')
            t = np.asarray(nodes[child].translation, float)
            if norm(m @ t - t) > TOLERANCE * max(1., norm(t)):
                raise ValueError(f'{side}.{key} does not fix the {child} translation axis')
            mats[key] = m
        result[side] = mats
    return raw, result


def corrections(calibration):
    """Node name -> (left, right) constant factors on the local rotation matrix."""
    out = {}
    for side, m in calibration.items():
        upper, fore, hand = chain(side)
        out[upper] = (np.eye(3), m['Cu'])
        out[fore] = (m['Cu'].T, m['Cf'])
        out[hand] = (m['Cf'].T, np.eye(3))
    return out


def read(path):
    stream = nif.NiStream(); stream.load(path)
    root = stream.root
    names = [e.string_data for e in root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = list(root.controllers)
    if len(names) != len(controllers):
        raise ValueError(f'{path}: {len(names)} channel names for {len(controllers)} controllers')
    return stream, names, controllers


def rotate_keys(keys, left, right):
    out = keys.copy()
    previous = None
    for row in out:
        q = matrix_quat(left @ quat_matrix(row[[2, 3, 4, 1]]) @ right)
        q = q / norm(q)
        if previous is not None and previous @ q < 0:
            q = -q
        row[1:5] = q
        previous = q
    out[:, 0] = keys[:, 0]
    return out


def signature(stream, names, controllers, skip_rotation):
    """Everything that must stay byte-identical, as comparable values."""
    root = stream.root
    extras = []
    for e in root.extra_datas:
        if isinstance(e, nif.NiTextKeyExtraData):
            extras.append(('text', [(float(t), str(k)) for t, k in e.keys]))
        else:
            extras.append((type(e).__name__, getattr(e, 'string_data', None)))
    channels = []
    for name, c in zip(names, controllers):
        d = c.data
        row = [name, type(c).__name__, int(c.flags), float(c.frequency), float(c.phase),
               float(c.start_time), float(c.stop_time)]
        for field in ('translations', 'scales'):
            data = getattr(d, field)
            row += [int(data.key_type), np.asarray(data.keys).tobytes()]
        rot = d.rotations
        row.append(int(rot.key_type))
        keys = np.asarray(rot.keys)
        row.append(keys[:, 0].tobytes() if name in skip_rotation else keys.tobytes())
        channels.append(row)
    return type(root).__name__, extras, channels


def calibrate_file(source, target, factors):
    stream, names, controllers = read(source)
    if len(set(names)) != len(names):
        raise ValueError(f'{source}: duplicate channel names')
    missing = [n for n in factors if n not in names]
    if missing:
        raise ValueError(f'{source}: missing arm channels {missing}')
    datas = [id(c.data) for c in controllers] + [id(c.data.rotations) for c in controllers]
    if len(set(datas)) != len(datas):
        raise ValueError(f'{source}: shared keyframe data between channels')
    counts, expected = {}, {}
    for name, c in zip(names, controllers):
        if name not in factors:
            continue
        rot = c.data.rotations
        if int(rot.key_type) != int(nif.NiFloatData.KeyType.LIN_KEY) or rot.euler_data:
            raise ValueError(f'{source}: {name} rotation format {rot.key_type!r} unsupported (need linear quaternions)')
        keys = np.asarray(rot.keys)
        if keys.ndim != 2 or keys.shape[1] != 5 or not len(keys) or not np.isfinite(keys).all():
            raise ValueError(f'{source}: {name} has no usable quaternion keys')
        new = rotate_keys(keys.astype(np.float64), *factors[name]).astype(keys.dtype)
        rot.keys = new
        expected[name] = new
        counts[name] = len(new)
    stream.save(target)
    # Verify by reload: output against the untouched input file.
    before = read(source); after = read(target)
    if signature(*before, set(factors)) != signature(*after, set(factors)):
        raise ValueError(f'{target}: preserved fields changed')
    for name, c in zip(after[1], after[2]):
        if name in factors:
            keys = np.asarray(c.data.rotations.keys)
            if keys.tobytes() != expected[name].tobytes():
                raise ValueError(f'{target}: {name} rotation keys did not round-trip')
            for q_in, q_out in zip(np.asarray(dict(zip(before[1], before[2]))[name].data.rotations.keys), keys):
                want = factors[name][0] @ quat_matrix(q_in[[2, 3, 4, 1]]) @ factors[name][1]
                if np.abs(quat_matrix(q_out[[2, 3, 4, 1]]) - want).max() > 1e-5:
                    raise ValueError(f'{target}: {name} key deviates from calibrated rotation')
    return counts


def refuse_prior(root):
    if any(p.name == REPORT for p in root.rglob('*.json')):
        raise ValueError('Input already contains an arm-frame calibration report')
    for path in root.rglob('*.json'):
        text = path.read_text(errors='replace')
        if f'"{MARKER}"' in text:
            raise ValueError(f'{path}: already marked {MARKER}')
        hits = [m for m in ANATOMY_MARKERS if m in text]
        if hits:
            raise ValueError(f'{path}: anatomical sword metadata {hits}; refuse double correction')


def kf_files(root):
    files = []
    for animations in [p for p in root.iterdir() if p.is_dir() and p.name.lower() == 'animations']:
        for folder in sorted(p for p in animations.iterdir() if p.is_dir() and p.name.lower() in FOLDERS):
            files += sorted(p for p in folder.iterdir() if p.is_file() and p.suffix.lower() == '.kf')
    return files


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--input', type=Path, required=True, help='Data root (firstperson-data, clinch-data, ...)')
    p.add_argument('--output', type=Path, required=True, help='Fresh output root; must not exist')
    p.add_argument('--calibration', type=Path, required=True, help='JSON {R:{Cu,Cf},L:{Cu,Cf}} 3x3 rotations')
    p.add_argument('--skeleton', type=Path, required=True, help='First-person skeleton NIF for axis validation')
    a = p.parse_args()
    root, out = a.input.resolve(), a.output.resolve()
    if out.exists():
        p.error('Output exists; use a fresh root')
    if out == root or root in out.parents:
        p.error('Output must be outside the input root')
    raw, calibration = load_calibration(a.calibration, a.skeleton)
    factors = corrections(calibration)
    refuse_prior(root)
    files = kf_files(root)
    if not files:
        raise ValueError(f'No first-person family KFs under {root}/Animations/{{{",".join(FOLDERS)}}}')
    shutil.copytree(root, out)
    report = dict(converter=HERE.name, mode='constant-rest-frame', input=str(root), output=str(out),
                  recipe_sha256=sha(HERE), calibration=dict(path=str(a.calibration), sha256=sha(a.calibration), value=raw),
                  skeleton=dict(path=str(a.skeleton), sha256=sha(a.skeleton)),
                  formula="U'=U*Cu; F'=Cu^T*F*Cf; H'=Cf^T*H (local rotation keys, both sides)",
                  preserved=['times', 'translations', 'scales', 'text keys', 'channel names/order',
                             'controller flags/frequency/phase/start/stop', 'all other bone rotations',
                             'skeleton and binds (not touched)', 'non-KF files (copied)'],
                  notes='Constant REST calibration of old generic-generated data; not a dynamic rest-relative '
                        'solve and not evidence of Source-anatomical correctness.',
                  files={})
    for source in files:
        rel = source.relative_to(root)
        target = out / rel
        counts = calibrate_file(source, target, factors)
        report['files'][rel.as_posix()] = dict(input_sha256=sha(source), output_sha256=sha(target),
                                               rotation_keys=counts)
    summary = dict(report=REPORT, mode='constant-rest-frame', recipe_sha256=report['recipe_sha256'],
                   calibration_sha256=report['calibration']['sha256'], files=len(report['files']))
    updated = []
    families = out / 'weapon-families-import.json'
    if families.exists():
        manifest = json.loads(families.read_text())
        outputs = manifest.get('outputs')
        if isinstance(outputs, dict):
            for rel, info in report['files'].items():
                if rel in outputs:
                    outputs[rel] = info['output_sha256']
        manifest[MARKER] = summary
        families.write_text(json.dumps(manifest, indent=1) + '\n'); updated.append(families.name)
    viewmodel = out / 'viewmodel-import.json'
    if viewmodel.exists():
        manifest = json.loads(viewmodel.read_text())
        manifest[MARKER] = summary
        viewmodel.write_text(json.dumps(manifest, indent=2) + '\n'); updated.append(viewmodel.name)
    report['updated_manifests'] = updated
    report['unreconciled_manifests'] = sorted(q.relative_to(out).as_posix() for q in out.rglob('*.json')
                                              if q.name not in updated)
    (out / REPORT).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(output=str(out), files=len(report['files']), updated_manifests=updated), indent=2))


if __name__ == '__main__':
    main()
