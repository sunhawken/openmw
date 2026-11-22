#!/usr/bin/env python3
"""Read creature and special NPC records, skeletons and animation groups from the user's own game files.

Writes derived facts (ids, bone names, group names) to a JSON file. No game data is copied.
"""
import argparse, collections, json, re, struct, sys, tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / 'third_party/io_scene_mw/lib'))
from es3 import nif  # noqa: E402

DEFAULT_GAME = None
DEFAULT_OUT = None
PLUGINS = ('Morrowind', 'Tribunal', 'Bloodmoon')
CREATURE_TYPES = {0: 'creature', 1: 'daedra', 2: 'undead', 3: 'humanoid'}


def records(path):
    data = path.read_bytes()
    pos = 0
    while pos < len(data):
        tag = data[pos:pos + 4].decode('latin1')
        size, _, flags = struct.unpack_from('<III', data, pos + 4)
        yield tag, flags, data[pos + 16:pos + 16 + size]
        pos += 16 + size


def subrecords(body):
    pos = 0
    while pos + 8 <= len(body):
        tag = body[pos:pos + 4].decode('latin1')
        size = struct.unpack_from('<I', body, pos + 4)[0]
        yield tag, body[pos + 8:pos + 8 + size]
        pos += 8 + size


def text(raw):
    return raw.split(b'\0', 1)[0].decode('cp1252', 'replace')


def norm(path):
    return path.replace('/', '\\').lower()


def read_bsa(path):
    with path.open('rb') as f:
        version, hash_offset, count = struct.unpack('<III', f.read(12))
        if version != 0x100:
            raise ValueError(f'{path}: not a TES3 BSA')
        table = struct.unpack(f'<{count * 2}I', f.read(count * 8))
        name_offsets = struct.unpack(f'<{count}I', f.read(count * 4))
        names = f.read(hash_offset - 12 * count)
    base = 12 + hash_offset + 8 * count
    files = {}
    for i in range(count):
        name = names[name_offsets[i]:names.index(b'\0', name_offsets[i])].decode('latin1')
        files[norm(name)] = (path, base + table[i * 2 + 1], table[i * 2])
    return files


def load_vfs(game):
    vfs = {}
    for plugin in PLUGINS:
        vfs.update(read_bsa(game / f'{plugin}.bsa'))
    for loose in (game / 'Meshes').rglob('*') if (game / 'Meshes').exists() else ():
        if loose.is_file():
            relative = norm(loose.relative_to(game).as_posix())
            vfs[relative] = (loose, 0, loose.stat().st_size)
    return vfs


def load_nif(vfs, name):
    entry = vfs.get(norm('meshes\\' + name))
    if not entry:
        return None
    path, offset, size = entry
    with path.open('rb') as f:
        f.seek(offset)
        raw = f.read(size)
    # A closed file in a temporary folder: Windows cannot reopen an open temporary file.
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder) / 'model.nif'
        temp.write_bytes(raw)
        stream = nif.NiStream()
        stream.load(str(temp))
    return stream


def actor_model_path(vfs, model):
    """Mirror OpenMW's correctActorModelPath: prefer the x-prefixed skeleton beside the model."""
    model = norm(model)
    head, _, tail = model.rpartition('\\')
    prefixed = (head + '\\' if head else '') + 'x' + tail
    return prefixed if norm('meshes\\' + prefixed) in vfs else model


def skeleton_facts(vfs, model):
    resolved = actor_model_path(vfs, model)
    stream = load_nif(vfs, resolved)
    if stream is None:
        return {'resolved': resolved, 'missing': True}
    nodes, shapes, skinned, skin_bones = {}, 0, 0, set()
    for root in stream.roots_of_type(nif.NiAVObject):
        stack = [(root, None)]
        while stack:
            obj, parent = stack.pop()
            if obj is None:
                continue
            if isinstance(obj, nif.NiNode):
                if obj.name:
                    nodes[obj.name] = parent
                for child in obj.children:
                    stack.append((child, obj.name or parent))
            elif isinstance(obj, nif.NiTriShape):
                shapes += 1
                if obj.skin is not None:
                    skinned += 1
                    skin_bones.update(b.name for b in obj.skin.bones if b is not None and b.name)
    return {'resolved': resolved, 'nodes': nodes, 'shapes': shapes, 'skinned': skinned,
            'skinBones': sorted(skin_bones)}


def animation_groups(vfs, model):
    """Group names from the text keys of the matching KF, which is where a creature's clips live."""
    resolved = actor_model_path(vfs, model)
    kf = re.sub(r'\.nif$', '.kf', resolved)
    stream = load_nif(vfs, kf)
    if stream is None:
        return None
    groups = set()
    for extra in stream.objects_of_type(nif.NiTextKeyExtraData):
        for value in extra.keys['f1'].tolist():
            for line in str(value).splitlines():
                found = re.match(r'\s*(.+?)\s*:\s*start\s*$', line, re.I)
                if found:
                    groups.add(found.group(1).lower())
    return sorted(groups)


def scan(game):
    creatures, npcs, placed, leveled = {}, {}, collections.Counter(), collections.Counter()
    for plugin in PLUGINS:
        for tag, _, body in records(game / f'{plugin}.esm'):
            if tag == 'CREA':
                entry = {'source': plugin, 'scale': 1.0}
                for sub, raw in subrecords(body):
                    if sub == 'NAME': entry['id'] = text(raw)
                    elif sub == 'MODL': entry['model'] = text(raw)
                    elif sub == 'FNAM': entry['name'] = text(raw)
                    elif sub == 'CNAM': entry['soundParent'] = text(raw)
                    elif sub == 'XSCL': entry['scale'] = struct.unpack('<f', raw)[0]
                    elif sub == 'FLAG': entry['flags'] = struct.unpack('<I', raw)[0]
                    elif sub == 'NPDT':
                        kind, level = struct.unpack_from('<ii', raw)
                        entry['type'] = CREATURE_TYPES.get(kind, str(kind))
                        entry['level'] = level
                        entry['health'] = struct.unpack_from('<i', raw, 40)[0]
                if 'id' in entry:
                    creatures[entry['id'].lower()] = entry
            elif tag == 'NPC_':
                entry = {'source': plugin, 'model': ''}
                for sub, raw in subrecords(body):
                    if sub == 'NAME': entry['id'] = text(raw)
                    elif sub == 'MODL': entry['model'] = text(raw)
                    elif sub == 'FNAM': entry['name'] = text(raw)
                    elif sub == 'RNAM': entry['race'] = text(raw)
                    elif sub == 'FLAG': entry['flags'] = struct.unpack('<I', raw)[0]
                if 'id' in entry:
                    npcs[entry['id'].lower()] = entry
            elif tag == 'CELL':
                for sub, raw in subrecords(body):
                    if sub == 'NAME':
                        placed[text(raw).lower()] += 1
            elif tag == 'LEVC':
                for sub, raw in subrecords(body):
                    if sub == 'CNAM':
                        leveled[text(raw).lower()] += 1
    return creatures, npcs, placed, leveled


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    creatures, npcs, placed, leveled = scan(args.game)
    vfs = load_vfs(args.game)
    models, errors = {}, {}
    wanted = {norm(c['model']) for c in creatures.values() if c.get('model')}
    wanted |= {norm(n['model']) for n in npcs.values() if n.get('model')}
    for model in sorted(wanted):
        try:
            facts = skeleton_facts(vfs, model)
            facts['groups'] = animation_groups(vfs, model)
            models[model] = facts
        except Exception as error:  # one unreadable mesh must not hide the others
            errors[model] = repr(error)
            models[model] = {'error': repr(error)}
    for table in (creatures, npcs):
        for key, entry in table.items():
            entry['placed'] = placed.get(key, 0)
            entry['leveled'] = leveled.get(key, 0)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps({'creatures': creatures, 'npcs': npcs, 'models': models}, indent=1, sort_keys=True))
    special = [n for n in npcs.values() if n['model']]
    print(f'creatures={len(creatures)} npcs={len(npcs)} npcsWithModel={len(special)} models={len(models)} errors={len(errors)}')
    for model, error in errors.items():
        print('ERROR', model, error)


if __name__ == '__main__':
    main()
