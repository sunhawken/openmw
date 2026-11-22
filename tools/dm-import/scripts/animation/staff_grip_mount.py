#!/usr/bin/env python3
"""Move native polearms along the haft without changing their arm poses.

Reads a first-person data directory and writes a fresh one. Only Weapon Bone
translation keys in brutal_fp_staff change, so the staff attack-chain timestamps
and source regrips stay intact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

import numpy as np

from retarget_kf import norm
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'third_party/io_scene_mw/lib'))
from es3 import nif
from import_weapon_families import STAFF_MOUNT_OFFSET
from retarget_kf import quat_matrix


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch(source, output, offset):
    if output.exists():
        raise ValueError('The output directory must not exist yet: '+str(output))
    manifest_path = source/'weapon-families-import.json'
    manifest = json.loads(manifest_path.read_text())
    previous = float(manifest.get('staff', {}).get('staff_mount_offset', 0.))
    delta = offset-previous
    shutil.copytree(source, output)
    rows = []
    for path in sorted((output/'Animations/brutal_fp_staff').glob('*.kf')):
        stream = nif.NiStream(); stream.load(path)
        names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
        index = names.index('Weapon Bone')
        controller = list(stream.root.controllers)[index]
        rotations = controller.data.rotations.keys
        positions = controller.data.translations.keys.copy()
        if rotations.shape[1] != 5 or positions.shape[1] != 4:
            raise ValueError('Expected linear imported rotation/translation keys: '+str(path))
        original = positions.copy()
        for i, time in enumerate(positions[:, 0]):
            key = int(np.argmin(abs(rotations[:, 0]-time)))
            if abs(float(rotations[key, 0]-time)) > 1e-5:
                raise ValueError('Weapon rotation and translation clocks differ: '+str(path))
            q = rotations[key, 1:]
            positions[i, 1:] += quat_matrix(q[[1, 2, 3, 0]])@np.array([0., delta, 0.])
        controller.data.translations.keys = positions
        stream.save(path)
        check = nif.NiStream(); check.load(path)
        old = nif.NiStream(); old.load(source/path.relative_to(output))
        preserved = 0
        for name, before, after in zip(names, old.root.controllers, check.root.controllers):
            for channel in ('rotations', 'translations', 'scales'):
                a, b = getattr(before.data, channel).keys, getattr(after.data, channel).keys
                if name == 'Weapon Bone' and channel == 'translations':
                    if a[:, 0].tobytes() != b[:, 0].tobytes():
                        raise ValueError('Weapon timestamps changed: '+str(path))
                    continue
                if a.tobytes() != b.tobytes():
                    raise ValueError('Arm, camera or finger key changed: '+str(path)+':'+name+':'+channel)
                preserved += len(a)
        text_before = next(e for e in old.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData)).keys
        text_after = next(e for e in check.root.extra_datas if isinstance(e, nif.NiTextKeyExtraData)).keys
        if list(text_before) != list(text_after):
            raise ValueError('Text keys changed: '+str(path))
        rows.append(dict(file=path.relative_to(output).as_posix(), keys=len(positions),
                         other_channel_keys_byte_identical=preserved,
                         source_sha256=sha(source/path.relative_to(output)), output_sha256=sha(path),
                         max_translation=float(np.max(norm(positions[:, 1:]-original[:, 1:], axis=1)))))
    manifest['staff']['staff_mount_offset'] = offset
    manifest['staff']['staff_mount_axis'] = 'Weapon Bone local +Y; hands unchanged'
    for row in rows:
        manifest.setdefault('outputs', {})[row['file']] = row['output_sha256']
    (output/'weapon-families-import.json').write_text(json.dumps(manifest, indent=1)+'\n')
    report = dict(source=str(source.resolve()), output=str(output.resolve()),
                  previous_offset=previous, offset=offset, delta=delta,
                  axis='Weapon Bone local +Y', rotations='unchanged', arms='unchanged',
                  text_keys='unchanged', camera='unchanged', rows=rows,
                  source_recipe_sha256=sha(Path(__file__)))
    (output/'staff-grip-mount-import.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--offset', type=float, default=STAFF_MOUNT_OFFSET)
    a = p.parse_args()
    if not np.isfinite(a.offset):
        p.error('Offset must be finite')
    report = patch(a.input, a.output, a.offset)
    print(json.dumps(dict(output=report['output'], groups=len(report['rows']), offset=a.offset,
                          preserved_keys=sum(row['other_channel_keys_byte_identical'] for row in report['rows'])), indent=2))


if __name__ == '__main__':
    main()
