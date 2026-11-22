#!/usr/bin/env python3
"""Replace polearm parry flourishes used as wall reactions with a calm held pose.

The native response supplies contact-dependent withdrawal and folding. A parry
flourish is unsuitable here: its shaft crosses the eye and forces discontinuous
clearance compromises. Keep all events, durations and ordinary attacks exact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

import numpy as np

from audit_staff_mesh_grip import sample_channels
from audit_viewmodel_continuity import tracks
from stabilize_staff_left_windup import load


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch(source, output):
    if output.exists():
        raise ValueError('The output directory must not exist yet')
    from es3 import nif
    relative = Path('Animations/brutal_fp_staff/weapononehand.kf')
    before = tracks(source / relative)
    stream = load(source / relative)
    marks = [(float(t), str(k)) for t, k in stream.root.extra_data.keys]
    keys = {k.removeprefix('weapononehand: '): t for t, k in marks}
    names = [e.string_data for e in stream.root.extra_datas if isinstance(e, nif.NiStringExtraData)]
    controllers = dict(zip(names, stream.root.controllers))
    idle = sample_channels(tracks(source / relative.parent / 'idle1h.kf'), 0.)
    intervals = [(keys[key + ' start'], keys[key + ' stop']) for key in ('bounce', 'bouncel', 'bouncer')]
    for name, controller in controllers.items():
        for channel, value in zip(('rotations', 'translations'), idle[name]):
            rows = getattr(controller.data, channel).keys
            selected = np.zeros(len(rows), bool)
            for start, stop in intervals:
                selected |= (rows[:, 0] >= start) & (rows[:, 0] <= stop)
            rows[selected, 1:] = value
    shutil.copytree(source, output)
    stream.save(output / relative)
    after = tracks(output / relative)
    for name in before:
        for a, b in zip(before[name], after[name]):
            keep = np.ones(len(a), bool)
            for start, stop in intervals:
                keep &= (a[:, 0] < start) | (a[:, 0] > stop)
            assert np.array_equal(a[:, 0], b[:, 0])
            assert np.array_equal(a[keep], b[keep])
    assert marks == [(float(t), str(k)) for t, k in load(output / relative).root.extra_data.keys]
    rules = relative.with_suffix('.yaml')
    text = (source / rules).read_text()
    text, count = re.subn(r"(to: 'weapononehand:bounce[lr]? start'\n    easing: linear\n    duration: )0\.06\n",
                          r'\g<1>0.16\n', text)
    assert count == 3, count
    (output / rules).write_text(text)
    report = dict(source=str(source.resolve()), output=str(output.resolve()),
                  reaction='native contact withdrawal on a calm two-hand pose',
                  entry_blend_seconds=.16, intervals=intervals,
                  all_other_channel_values_and_event_clocks_exact=True,
                  outputs={p.as_posix(): sha(output / p) for p in (relative, rules)},
                  recipe_sha256=sha(Path(__file__)))
    manifest = json.loads((source / 'weapon-families-import.json').read_text())
    manifest['outputs'].update(report['outputs'])
    manifest['staff']['anchored_wall_recoil'] = report
    (output / 'weapon-families-import.json').write_text(json.dumps(manifest, indent=1) + '\n')
    (output / 'staff-anchored-wall-import.json').write_text(json.dumps(report, indent=2) + '\n')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(patch(args.input, args.output), indent=2))
