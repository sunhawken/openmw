#!/usr/bin/env python3
"""List Source MDL skeletons and clips; optionally decode a clip to JSON.

Requires the SourceIO checkout at third_party/SourceIO (or --sourceio). The
output contains game data.
"""
import argparse
import json
from pathlib import Path
import struct
import sys


def load_mdl(path, sourceio):
    sys.path.insert(0, str(sourceio.parent.resolve()))
    from SourceIO.library.models.mdl.structs.header import MdlHeaderV44
    from SourceIO.library.models.mdl.structs.bone import Bone
    from SourceIO.library.models.mdl.structs.local_animation import StudioAnimDesc
    from SourceIO.library.utils import MemoryBuffer
    raw = path.read_bytes()
    magic, version = struct.unpack_from('<4sI', raw)
    if magic != b'IDST' or version not in range(44, 50):
        raise ValueError(f'Unsupported header {magic!r}, version {version}')
    buf = MemoryBuffer(raw)
    header = MdlHeaderV44.from_buffer(buf)
    if header.file_size != len(raw):
        raise ValueError(f'Header size {header.file_size} differs from file size {len(raw)}')
    buf.seek(header.bone_offset)
    bones = []
    for index in range(header.bone_count):
        bone = Bone.from_buffer(buf, version)
        bone.bone_id = index
        bones.append(bone)
    buf.seek(header.local_animation_offset)
    animations = [StudioAnimDesc.from_buffer(buf) for _ in range(header.local_animation_count)]
    return buf, header, bones, animations


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('mdl', type=Path)
    ap.add_argument('--sourceio', type=Path, default=Path(__file__).resolve().parents[2] / 'third_party/SourceIO')
    ap.add_argument('--decode', help='Exact local animation name to decode')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    buf, header, bones, animations = load_mdl(args.mdl, args.sourceio)
    result = {
        'file': str(args.mdl.resolve()), 'version': header.version, 'model': header.name,
        'bones': [{'name': b.name, 'parent': b.parent_id, 'position': b.position, 'quat_xyzw': b.quat} for b in bones],
        'animations': [{'name': a.name, 'fps': a.fps, 'frames': a.frame_count, 'flags': int(a.flags),
                        'external_block': a.animblock_id} for a in animations],
    }
    if args.decode:
        anim = next(a for a in animations if a.name == args.decode)
        if anim.animblock_id:
            raise ValueError('External animation block requires .ani block-table support')
        frames = anim.read_animations(buf, bones)
        if not frames:
            raise ValueError('Animation decoder returned no tracks')
        result['clip'] = {'name': anim.name, 'fps': anim.fps, 'frames': anim.frame_count, 'flags': int(anim.flags),
                          'tracks': {n: {'positions': t['pos'].tolist(), 'rotations_xyzw': t['rot'].tolist()} for n, t in frames.items()}}
    output = json.dumps(result, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output + '\n')
        print(f'{header.name}: {len(bones)} bones, {len(animations)} clips -> {args.output}')
    else:
        print(output)

if __name__ == '__main__':
    main()
