#!/usr/bin/env python3
"""Import Dark Messiah's footstep, landing and jump sounds with their sound-script values.

Writes the sounds to sound/brutal_dm/steps/ and a table, brutal/footsteps.txt, that the
engine reads. Each table line is

    event <tab> volume min <tab> volume max <tab> pitch min <tab> pitch max <tab> file;file...

Player events are Dark Messiah's surface events, "<surface>.step<side><gait><jump>"
(gait: empty, stealth, run, sprint; jump: the landing variant). NPC events are the
character-type events "guard.footstep<side>" and "villagermale.footstep<side>".
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import sys
import wave

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/animation'))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from dm_sources import archives, entries, extract  # noqa: E402
from import_dark_messiah_effects import leaves  # noqa: E402

SURFACES = ('concrete', 'wood', 'sand', 'metal', 'dirt', 'grass', 'carpet', 'water', 'wade', 'mud', 'chitin',
            'slate', 'glass')
GAITS = ('', 'stealth', 'run', 'sprint')
NPC_TYPES = ('guard', 'villagermale')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def keyvalues(text):
    """KeyValues reader that skips a stray closing brace at the top level, as the game does."""
    text = re.sub(r'//[^\n]*', '', text)
    tokens = [a or b or c for a, b, c in re.findall(r'"([^"\n]*)"|([{}])|([^\s{}"]+)', text)]
    pos = 0

    def block(nested):
        nonlocal pos
        result = []
        while pos < len(tokens):
            key = tokens[pos]
            pos += 1
            if key == '}':
                if nested:
                    return result
                continue
            if key == '{' or pos >= len(tokens):
                continue
            value = tokens[pos]
            pos += 1
            if value == '{':
                value = block(True)
            result.append((key, value))
        return result
    return block(False)


def pair(text, default, scale=1.):
    """Sound-script "a,b" or "a" ranges; named constants mean the default."""
    values = [v.strip() for v in str(text).split(',')]
    try:
        numbers = [float(v) / scale for v in values]
    except ValueError:
        return default, default
    return numbers[0], numbers[-1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--language', default='english')
    args = ap.parse_args()
    if not re.fullmatch('[a-z]+', args.language):
        raise ValueError('Invalid language')
    index = {}
    for archive in archives(args.vpk_root):
        for name, entry in entries(archive):
            index[name.lower()] = (archive, name, entry)
    if not index:
        raise ValueError('No Dark Messiah resources found')

    def read(name):
        archive, _, entry = index[name.lower()]
        return extract(archive, entry)

    events = {}
    for script in ('game_sounds_footsteps', 'game_sounds_player', 'npc_sounds_common'):
        for name, tree in keyvalues(read(f'mm/scripts/{script}.txt').decode('cp1252')):
            if isinstance(tree, list):
                events[name.lower()] = tree

    wanted = [f'{surface}.step{side}{gait}{jump}' for surface in SURFACES for side in ('left', 'right')
              for gait in GAITS for jump in ('', 'jump')]
    wanted += [f'{kind}.footstep{side}' for kind in NPC_TYPES for side in ('left', 'right')]
    wanted.append('player.jump')
    missing = [name for name in wanted if name not in events]
    if missing:
        raise ValueError('Dark Messiah sound scripts lack: ' + ', '.join(missing))

    outputs, lines, written = {}, [], {}
    for name in wanted:
        tree = events[name]
        values = {key.lower(): value for key, value in tree if isinstance(value, str)}
        volume = pair(values.get('volume', '1'), 1.)
        pitch = pair(values.get('pitch', '100'), 1., 100.)
        files = []
        for reference in leaves(tree, 'wave'):
            relative = re.sub(r'^[*#@~!><^)+?]+', '', reference).replace('\\', '/').replace('%language%', args.language)
            source = 'mm/sound/' + relative
            if source.lower() not in index:
                raise ValueError(f'{name}: missing {source}')
            target = 'sound/brutal_dm/steps/' + Path(relative).name.lower()
            if target not in written:
                data = read(source)
                with wave.open(io.BytesIO(data)) as wav:
                    if wav.getcomptype() != 'NONE':
                        raise ValueError(f'Unsupported compressed WAV {source}')
                dest = args.output / target
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(data)
                written[target] = source
                outputs[target] = sha(data)
            files.append(target)
        if not files:
            raise ValueError(f'{name}: no waves')
        lines.append('\t'.join([name, f'{volume[0]:g}', f'{volume[1]:g}', f'{pitch[0]:g}', f'{pitch[1]:g}',
                                ';'.join(files)]))

    table = args.output / 'brutal/footsteps.txt'
    table.parent.mkdir(parents=True, exist_ok=True)
    table.write_text('\n'.join(lines) + '\n')
    outputs['brutal/footsteps.txt'] = sha(table.read_bytes())
    (args.output / 'footsteps-manifest.json').write_text(json.dumps(
        dict(events=len(lines), sounds=dict(sorted(written.items())), outputs=dict(sorted(outputs.items()))),
        indent=1) + '\n')
    print(f'{len(lines)} events, {len(written)} sounds')


if __name__ == '__main__':
    main()
