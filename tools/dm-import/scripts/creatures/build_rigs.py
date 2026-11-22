#!/usr/bin/env python3
"""Plan a ragdoll rig for every creature model from its real bones and geometry.

Writes mod/brutal/creature_rigs.txt (bone names and numbers only, no game data) and a JSON
summary for the catalog. Capsules are stored in bone-local space, so they fit whatever pose
the creature is in when it dies.
"""
import argparse, json, sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import scan_game as scan  # noqa: E402
from nif_bind import Skeleton  # noqa: E402

PROJECT = Path(__file__).resolve().parents[2]
MAX_PARTS = 16
MIN_SIZE_RATIO = .17       # a part smaller than this share of the body merges into its parent
HUMAN_MASS, HUMAN_VOLUME = 64., 50000.  # the NPC ragdoll's mass, at this capsule volume
PINNED = {'bip01 pelvis', 'bip01 spine1', 'bip01 head', 'bip01 l upperarm', 'bip01 l forearm', 'bip01 r upperarm',
          'bip01 r forearm', 'bip01 l thigh', 'bip01 l calf', 'bip01 r thigh', 'bip01 r calf'}
# Scripted set pieces whose death is part of a quest scene: they keep their own animation.
EXCLUDED = {'r\\heart_akulakhan.nif': 'scripted set piece'}
LIMITS = (  # (name fragments, swing1, swing2, twist)
    # One rigid piece stands for a whole tentacle, so it needs a wide joint or the body stands on stilts.
    (('tent', 'tongue'), 85, 85, 40),
    (('forearm', 'lowerarm', 'elbow'), 70, 20, 15),
    (('calf', 'shin', 'knee', 'lowerleg'), 65, 15, 10),
    (('upperarm', 'clavicle', 'shoulder'), 70, 70, 35),
    (('thigh', 'hip', 'upperleg'), 55, 40, 15),
    (('head', 'jaw', 'neck'), 35, 35, 30),
    (('spine', 'pelvis', 'torso', 'body', 'chest'), 25, 25, 15),
    (('tail', 'wing', 'antena', 'antenna', 'rudder'), 40, 40, 25),
)
DEFAULT_LIMIT = (45, 35, 20)


def limits(bone):
    lowered = bone.lower()
    for names, swing1, swing2, twist in LIMITS:
        if any(name in lowered for name in names):
            return swing1, swing2, twist
    return DEFAULT_LIMIT


def choose_root(skeleton, owners):
    """The bone every other part hangs from: the pelvis, or the top of the heaviest subtree."""
    for name in skeleton.order:
        if name.lower() == 'bip01 pelvis':
            return name

    def ancestors(name):
        chain = [name]
        while skeleton.parent.get(chain[-1]):
            chain.append(skeleton.parent[chain[-1]])
        return chain
    chains = [ancestors(owner)[::-1] for owner in owners]
    shared = None
    for depth in range(min(map(len, chains))):
        if len({chain[depth] for chain in chains}) > 1:
            break
        shared = chains[0][depth]
    if shared in owners:
        return shared
    # No geometry at the shared ancestor: descend to the child holding the most vertices.
    counts = {}
    for owner, chain in zip(owners, chains):
        index = chain.index(shared) + 1 if shared in chain else 0
        if index < len(chain):
            counts[chain[index]] = counts.get(chain[index], 0) + len(skeleton.vertices[owner])
    return max(counts, key=counts.get) if counts else owners[0]


def plan_parts(skeleton):
    owners = [name for name in skeleton.order if name in skeleton.vertices]
    if not owners:
        return None
    root = choose_root(skeleton, owners)

    def nearest_part(name, parts):
        node = skeleton.parent.get(name)
        while node:
            if node in parts:
                return node
            node = skeleton.parent.get(node)
        return root
    parts = {name: {'bone': name, 'points': [skeleton.vertices[name]]} for name in owners}
    parts.setdefault(root, {'bone': root, 'points': []})
    # A biped keeps its anatomical bones even when armour and faces are modelled on nodes of
    # their own, as on Vivec, Golden Saints and Dremora.
    # A stray Bip01 name does not make a biped: a Riekling or a kagouti has only one or two of them.
    biped = sum(name.lower() in PINNED for name in skeleton.order) >= 8
    for name in skeleton.order:
        if biped and name.lower() in PINNED:
            parts.setdefault(name, {'bone': name, 'points': []})

    # A rigid mesh hung on a node beside a skinned body (a wolf's head, a guar's horns) has to
    # turn with the bone the skin follows, or a hole opens at the seam: the node is never a part.
    skinned = len(skeleton.skin_bones) >= 4

    has_children = {parent for parent in skeleton.parent.values() if parent}

    def rigid_holder(name):
        # A leaf: a skeleton's rigid limb bones also carry no skin, but they have bones below them.
        return skinned and name != root and name not in skeleton.skin_bones and name not in has_children \
            and not (biped and name.lower() in PINNED)

    def accessory(name):
        if rigid_holder(name):
            return True
        lowered = name.lower()
        if not biped or lowered in PINNED or name == root:
            return False
        # Spine segments, necks, clavicles, feet and hands belong to the pinned bone next to them;
        # other Bip01 chains (tails, wings) and everything else on a non-biped are judged by size.
        return biped and (not lowered.startswith('bip01')
                          or any(key in lowered for key in ('spine', 'neck', 'clavicle', 'foot', 'hand', 'toe', 'finger')))
    # Merging a pinned or sized bone's small extras leaves the chain anatomically connected.
    low, high = skeleton.extent()
    body = float(np.linalg.norm(high - low))

    def size(part):
        if not part['points']:
            return 0.
        points = np.concatenate(part['points'])
        return float(np.linalg.norm(points.max(0) - points.min(0)))

    def mergeable():
        return [name for name in parts if name != root and not (biped and name.lower() in PINNED)
                and not parts[name].get('holds')]
    # A holder's weight belongs to the bone above it, and that bone must exist as a part to take it:
    # a skinned collar on 'Head' then moves with the rigid head instead of staying with the neck.
    for name in [name for name in skeleton.order if name in parts and rigid_holder(name)]:
        bone = skeleton.parent.get(name)
        # The file's top node is not a bone the engine can pose.
        if bone and skeleton.parent.get(bone):
            parts.setdefault(bone, {'bone': bone, 'points': [], 'holds': True})
            parts[bone]['holds'] = True
    for name in [name for name in skeleton.order[::-1] if name in parts and accessory(name)]:
        parts[nearest_part(name, parts)]['points'].extend(parts[name]['points'])
        del parts[name]
    while True:
        candidates = mergeable()
        if not candidates:
            break
        smallest = min(candidates, key=lambda name: size(parts[name]))
        if size(parts[smallest]) >= MIN_SIZE_RATIO * body and len(parts) <= MAX_PARTS:
            break
        target = nearest_part(smallest, parts)
        parts[target]['points'].extend(parts[smallest]['points'])
        del parts[smallest]
    ordered = [name for name in skeleton.order if name in parts]
    ordered.remove(root)
    ordered.insert(0, root)
    return [(name, None if name == root else nearest_part(name, parts), parts[name]['points']) for name in ordered], body, biped


def capsule(skeleton, bone, point_sets, fallback):
    """A capsule in bone-local space: along the principal axis of the part's vertices."""
    world = skeleton.world[bone]
    scale = float(np.cbrt(abs(np.linalg.det(world[:3, :3])))) or 1.
    if point_sets:
        points = np.concatenate(point_sets)
        local = (points - world[:3, 3]) @ world[:3, :3] / (scale * scale)
    else:
        local = np.zeros((1, 3))
    if len(local) < 4:
        return (local.mean(0), np.array([0., 1., 0.]), .1, max(fallback, 1.5), scale)
    center = local.mean(0)
    _, _, axes = np.linalg.svd(local - center, full_matrices=False)
    axis = axes[0]
    along = (local - center) @ axis
    mid = (along.max() + along.min()) / 2
    center = center + axis * mid
    half = float((along.max() - along.min()) / 2)
    away = local - center
    radial = np.linalg.norm(away - np.outer(away @ axis, axis), axis=1)
    radius = float(np.percentile(radial, 70))
    radius = float(np.clip(radius, 1.5, max(half, 1.5)))
    return center, axis, max(half - radius, .1), radius, scale


def build(skeleton):
    planned = plan_parts(skeleton)
    if planned is None:
        return None
    plan, body, biped = planned
    parts, index = [], {}
    shapes = []
    for name, parent, points in plan:
        center, axis, half, radius, scale = capsule(skeleton, name, points, body * .06)
        world_radius, world_half = radius * scale, half * scale
        volume = np.pi * world_radius ** 2 * (2 * world_half) + 4 / 3 * np.pi * world_radius ** 3
        shapes.append((name, parent, center, axis, half, radius, world_radius, world_half, volume, scale))
    total_volume = sum(shape[-2] for shape in shapes)
    # Sublinear: a body twice the volume is not twice as hard to shove in play.
    total_mass = HUMAN_MASS * (total_volume / HUMAN_VOLUME) ** .75
    for name, parent, center, axis, half, radius, world_radius, world_half, volume, scale in shapes:
        # The engine multiplies by the bone's scale, which already holds the mesh's own; keep only the reference's.
        mass = float(np.clip(total_mass * volume / total_volume, 1., 120.)) / scale ** 2.25
        if parent is not None:
            parent_mass = parts[index[parent]]['mass']
            # Bullet's joints stay stable only for moderate mass ratios along a chain.
            mass = float(np.clip(mass, parent_mass / 6, parent_mass * 3))
        index[name] = len(parts)
        swing1, swing2, twist = limits(name)
        if half > radius * 6:
            # One rigid piece stands for a whole long tentacle or tail, whatever its bone is called:
            # with a narrow joint the body would stand on it as on a stilt.
            swing1, swing2, twist = 85, 85, 40
        if not biped and any(key in name.lower() for key in ('head', 'neck', 'jaw')):
            # Several neck links share the turn, and a seam shows long before the limit does.
            swing1, swing2, twist = 18, 18, 12
        parts.append({'bone': name, 'parent': -1 if parent is None else index[parent], 'center': center.tolist(),
                      'axis': axis.tolist(), 'half': half, 'radius': radius, 'worldRadius': world_radius,
                      'worldHalf': world_half, 'mass': mass, 'limits': [swing1, swing2, twist]})
    return parts


def number(value):
    return f'{value:.4f}'.rstrip('0').rstrip('.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--scan', type=Path, required=True)
    parser.add_argument('--rigs', type=Path, required=True, help='rig table the engine reads')
    parser.add_argument('--summary', type=Path, required=True, help='JSON summary for the catalog')
    args = parser.parse_args()
    facts = json.loads(args.scan.read_text())
    vfs = scan.load_vfs(args.game)
    models = sorted({scan.norm(c['model']) for c in facts['creatures'].values() if c.get('model')})
    lines = ['# Generated by scripts/creatures/build_rigs.py. Bone names and numbers only.',
             '# rig<TAB>model<TAB>parts; then per part: bone parent cx cy cz ax ay az halfLength radius mass swing1 swing2 twist']
    summary = {}
    for model in models:
        if model in EXCLUDED:
            summary[model] = {'excluded': EXCLUDED[model]}
            continue
        skeleton = Skeleton(scan.load_nif(vfs, scan.actor_model_path(vfs, model)))
        parts = build(skeleton)
        if not parts:
            summary[model] = {'excluded': 'no geometry on any bone'}
            continue
        low, high = skeleton.extent()
        summary[model] = {'parts': len(parts), 'bones': [p['bone'] for p in parts],
                          'mass': round(sum(p['mass'] for p in parts), 1),
                          'size': [round(float(x), 1) for x in high - low]}
        lines.append(f'rig\t{model}\t{len(parts)}')
        for part in parts:
            values = part['center'] + part['axis'] + [part['half'], part['radius'], part['mass']] + part['limits']
            lines.append('\t'.join([part['bone'], str(part['parent'])] + [number(float(v)) for v in values]))
    args.rigs.parent.mkdir(parents=True, exist_ok=True)
    args.rigs.write_text('\n'.join(lines) + '\n')
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(json.dumps(summary, indent=1, sort_keys=True))
    rigged = [s for s in summary.values() if 'parts' in s]
    print(f'models={len(models)} rigged={len(rigged)} excluded={len(models) - len(rigged)}')
    for model, entry in sorted(summary.items()):
        if 'excluded' in entry:
            print(' EXCLUDED', model, entry['excluded'])
        else:
            print(f" {model:32} parts={entry['parts']:2} mass={entry['mass']:6} size={entry['size']}")


if __name__ == '__main__':
    main()
