#!/usr/bin/env python3
"""Add footstep keys to the imported NPC walk and run loops.

Dark Messiah plays an NPC footstep from animation events 6004 (left foot) and 6005 (right
foot) in the guard's movement sequences. This step puts Morrowind's "soundgen: left/right"
keys at the same points of each loop, so NPCs step at Dark Messiah's pace. Files are
changed in place.
"""
import argparse
from pathlib import Path
import re
import sys

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'third_party/io_scene_mw/lib'))
from es3 import nif  # noqa: E402

# Fraction of the loop for the left and right foot: npc_guard_combat_animations.mdl Walk,
# Combatwalk (the armed walk) and Run.
PHASES = {'walk': (.278, .75), 'walk1h': (.219, .719), 'run': (.292, .625)}
GROUP = re.compile(r'(walk|run)(forward|back|left|right)(1h)?')


def add_keys(path, phases):
    stream = nif.NiStream()
    stream.load(path)
    extra = stream.root.extra_data
    keys = [(float(t), str(k)) for t, k in extra.keys]
    if any(k.lower().startswith('soundgen:') for _, k in keys):
        raise ValueError(f'{path} already has footstep keys')
    group = path.stem
    marks = {k.split(': ', 1)[1]: t for t, k in keys if k.startswith(group + ': ')}
    start, stop = marks.get('loop start'), marks.get('loop stop')
    if start is None or stop is None or stop <= start:
        raise ValueError(f'{path} has no loop')
    left, right = phases
    keys += [(start + left * (stop - start), 'soundgen: left'), (start + right * (stop - start), 'soundgen: right')]
    extra.keys = np.array(sorted(keys, key=lambda x: x[0]), dtype='<f,O')
    stream.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('data', type=Path, help='NPC data folder holding Animations/brutal_npc/*.kf')
    args = ap.parse_args()
    changed = 0
    for path in sorted((args.data / 'Animations/brutal_npc').glob('*.kf')):
        match = GROUP.fullmatch(path.stem)
        if match:
            mode = match.group(1)
            add_keys(path, PHASES[mode + ('1h' if mode == 'walk' and match.group(3) else '')])
            changed += 1
    if not changed:
        raise ValueError('No NPC walk or run loops found')
    print(f'Footstep keys in {changed} loops')


if __name__ == '__main__':
    main()
