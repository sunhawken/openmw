#!/usr/bin/env python3
"""Import model blood decals from the user's Dark Messiah VPKs.

RGBA bytes and material metadata are kept as they are; neutral-grey
multiplicative blending is implemented by the engine.
"""
import zlib
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/animation'))
from dm_sources import archives, entries, extract
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from pins import pinned_commit  # noqa: E402

PIN = '6deac3176bf0e8dd2a50038cfa1066abfa58c1f9'
MATERIALS = {'slash': 'decals/flesh/blood4', 'small': 'decals/flesh/blood2',
             'big': 'decals/flesh/blood1', 'tiny': 'decals/flesh/blood3'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--vpk-root', type=Path, required=True)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sourceio', type=Path, default=ROOT / 'third_party/SourceIO')
    args = ap.parse_args()
    if args.output.resolve().is_relative_to(ROOT):
        ap.error('Extracted game assets must go outside the importer folder')
    actual = pinned_commit(args.sourceio)
    if actual != PIN:
        raise ValueError(f'SourceIO pin mismatch: {actual}')
    sys.path.insert(0, str(args.sourceio.resolve().parent))
    from vtf_decode import load_vtf_texture_frames
    from PIL import Image
    index = {}
    for archive in archives(args.vpk_root):
        for name, entry in entries(archive):
            index[name.lower()] = archive, name, entry
    sources, outputs = {}, {}

    def read(name):
        archive, exact, entry = index[name.lower()]
        data = extract(archive, entry)
        sources[exact] = dict(archive=archive.name, crc32=f'{zlib.crc32(data):08x}',
                             sha256=hashlib.sha256(data).hexdigest())
        return data

    for name, material in MATERIALS.items():
        vmt_path = f'mm/materials/{material}.vmt'
        vmt = re.sub(r'//[^\n]*', '', read(vmt_path).decode('cp1252'))
        props = dict(re.findall(r'"(\$[^\"]+)"\s+"?([^\s"{}]+)"?', vmt))
        if not vmt.lstrip().lower().startswith('"decalmodulate"'):
            raise ValueError('Unexpected original decal shader')
        source = f'mm/materials/{props["$basetexture"].lower()}.vtf'
        raw = read(source)
        frames, width, height, floating = load_vtf_texture_frames(raw)
        if floating or len(frames) != 1 or struct.unpack_from('<H', raw, 24)[0] != 1:
            raise ValueError('Expected one static RGBA decal')
        picture = Image.frombytes('RGBA', (width, height), frames[0])
        buffer = io.BytesIO()
        picture.save(buffer, format='PNG')
        data = buffer.getvalue()
        if Image.open(io.BytesIO(data)).convert('RGBA').tobytes() != frames[0]:
            raise ValueError('PNG round trip failed')
        relative = f'textures/brutal_dm/model_blood_{name}.png'
        path = args.output / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        outputs[name] = dict(path=relative, sha256=hashlib.sha256(data).hexdigest(),
                             source=source, material=vmt_path, properties=props,
                             width=width, height=height, rgba_extrema=picture.getextrema())
        print(name, width, height, picture.getextrema())
    (args.output / 'model-blood-manifest.json').write_text(json.dumps(dict(
        schema=1, derived_from_game_files=True, sourceio=PIN, sources=sources, textures=outputs,
        pixel_orientation='Original decoded rows retained; engine ImageManager applies normal PNG orientation',
        blend='DecalModulate: DST_COLOR + SRC_COLOR, neutral RGB128; original alpha masks coverage'), indent=2) + '\n')


if __name__ == '__main__':
    main()
