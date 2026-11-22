#!/usr/bin/env python3
"""The plain kick (Dark Messiah @attack_kick_medium) for kicks without a one-handed weapon.

The same clip serves the third-person and the first-person skeleton.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ANIMATION = Path(__file__).resolve().parents[0] / 'scripts/animation'
sys.path.insert(0, str(ANIMATION))
from dm_sources import archives, entries, extract  # noqa: E402

MODEL = 'mm/models/npc/player/player_animations.mdl'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vpk-root', type=Path, required=True)
    parser.add_argument('--skeleton', type=Path, required=True, help="Morrowind's xbase_anim.nif")
    parser.add_argument('--work-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.work_dir.mkdir(parents=True, exist_ok=True)
    model = args.work_dir / 'player_animations.mdl'
    for vpk in reversed(archives(args.vpk_root)):
        found = next((entry for name, entry in entries(vpk) if name.lower() == MODEL), None)
        if found is not None:
            model.write_bytes(extract(vpk, found))
            break
    else:
        raise SystemExit(f'{MODEL} not found in the Dark Messiah archives')
    clip = args.work_dir / 'kick-decoded.json'
    subprocess.run([sys.executable, ANIMATION / 'inspect_mdl.py', model, '--decode', '@attack_kick_medium',
                    '--output', clip], check=True)
    third = args.output / 'Animations/xbase_anim/brutal_kick.kf'
    third.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, ANIMATION / 'retarget_kf.py', clip, args.skeleton, third,
                    '--map', ANIMATION / 'dark_messiah_bones.json', '--reference-frame', '0', '--yaw', '90'],
                   check=True)
    first = args.output / 'Animations/xbase_anim.1st/brutal_kick.kf'
    first.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(third, first)


if __name__ == '__main__':
    main()
