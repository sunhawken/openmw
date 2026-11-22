#!/usr/bin/env python3
"""Check native hand skins against anatomical dagger mounts.

Open hand/cuff boundaries make a single ray parity depend on ray direction.
Generalized solid-angle winding classifies the interior without that false hit.
"""
import argparse
import json
from pathlib import Path
import numpy as np
from retarget_kf import cross, norm
from audit_dagger_anatomy import sample
from audit_dagger_rendered_items import primary_commands
from audit_slide_pose import distances
from audit_staff_mesh_grip import weapon_triangles, surface_samples
from audit_viewmodel_continuity import tracks
from import_dagger_anatomy import DaggerAnatomy, FRAME_NODES
from import_viewmodel import Viewmodel, to4


class Hand:
    def __init__(self, nif, path, side):
        scene = nif.NiStream(); scene.load(path)
        self.parts = []
        for shape in scene.objects_of_type(nif.NiTriShape):
            if not shape.name.startswith('Tri ' + ('Left' if side == 'L' else 'Right') + ' Hand') or not shape.skin:
                continue
            self.parts.append((np.asarray(shape.data.vertices, float), np.asarray(shape.data.triangles),
                [(bone.name, data.vertex_weights['f0'], data.vertex_weights['f1'], data.matrix)
                 for bone, data in shape.bone_influences]))
        if not self.parts: raise ValueError('No skinned hand in ' + str(path))

    def triangles(self, world):
        parts = []
        for original, faces, influences in self.parts:
            points = np.zeros_like(original)
            for name, indices, weights, bind in influences:
                transform = world[name] @ bind
                points[indices] += weights[:, None] * (original[indices] @ transform[:3, :3].T + transform[:3, 3])
            parts.append(points[faces])
        return np.concatenate(parts)


def winding_penetration(triangles, points):
    winding = []
    for chunk in np.array_split(points, max(1, int(np.ceil(len(points) / 256)))):
        a, b, c = (triangles[None, :, index] - chunk[:, None] for index in range(3))
        na, nb, nc = (norm(value, axis=2) for value in (a, b, c))
        numerator = np.einsum('pti,pti->pt', a, cross(b, c))
        denominator = (na * nb * nc + np.einsum('pti,pti->pt', a, b) * nc
                       + np.einsum('pti,pti->pt', b, c) * na + np.einsum('pti,pti->pt', c, a) * nb)
        winding.extend(np.abs(np.sum(2 * np.arctan2(numerator, denominator), axis=1)) / (4 * np.pi))
    inside = np.asarray(winding) > .5
    depths = [float(distances(triangles, point).min()) for point in points[inside]]
    return dict(inside_samples=len(depths), max_depth=max(depths, default=0.))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('data', 'vpk-root', 'work-dir', 'skeleton', 'grip-animation', 'camera-contract', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--hand-mesh', required=True, type=Path, action='append')
    parser.add_argument('--weapon-mesh', required=True, type=Path, action='append')
    args = parser.parse_args()
    vm = DaggerAnatomy(args.vpk_root, args.work_dir, args.skeleton, args.grip_animation,
                       camera_contract=args.camera_contract, native_eye_height=True)
    vm.grip_path = args.grip_animation; vm.configure('dagger')
    declaration = json.loads((args.data / 'weapon-families-import.json').read_text())['dagger']
    profiles = declaration['anatomy_grip_mount']
    groups = declaration['groups']['brutal_fp_dagger']
    channels = {group: tracks(args.data / 'Animations/brutal_fp_dagger' / (group + '.kf'))
                for group in ('idle1h', 'brutal_block', 'weapononehand')}
    hands = {(side, str(path)): Hand(vm.nif, path, side) for side in 'LR' for path in args.hand_mesh}
    guards = {}
    for path in args.weapon_mesh:
        triangles = weapon_triangles(path)
        # Includes the broad crossguard and short blade root, excludes the grasped hilt.
        guards[str(path)] = surface_samples(triangles[(triangles[:, :, 1].min(1) >= 0.)
                                                     & (triangles[:, :, 1].max(1) <= 6.)])
    forward = vm.item_matrix('@right_down_to_up', 1000.)
    def delta(side, direction, elapsed=1000.):
        name = '@' + ('left' if side == 'L' else 'right') + '_' + direction
        return vm.to_weapon @ vm.item_matrix(name, elapsed) @ forward.T @ vm.to_weapon.T
    segments = groups['weapononehand']['segments']
    c = next(s for s in segments if s['clip'] == 'anm_fastattack_daggers_C')
    d = next(s for s in segments if s['clip'] == 'anm_fastattack_daggers_D')
    e = next(s for s in segments if s['clip'] == 'anm_fastattack_daggers_E')
    commands = primary_commands(vm, 'weapononehand', groups['weapononehand'])
    e_forward = {side: next(time for time, own, target, fast, reset in commands
                           if own == side and target == 'forward'
                           and e['offset'] <= time <= e['offset'] + .2) for side in 'LR'}
    poses = [('idle', sample(channels['idle1h'], 0.), None),
             ('guard', sample(channels['brutal_block'], .7), 'reverse'),
             ('fastD-blend', Viewmodel.blend_pose(sample(channels['weapononehand'], c['offset'] + c['duration']),
               sample(channels['weapononehand'], d['offset'] + .133331554877), .133331554877 / .2), 'reverse'),
             ('user-fastE', sample(channels['weapononehand'], 9.200965881), 'E')]
    times = channels['weapononehand']['Camera'][0][:, 0]
    for segment in segments:
        if not (segment['clip'].startswith('anm_bigslash_Daggers') and segment['clip'].endswith('_idle')): continue
        for time in times[(times >= segment['offset'] - 1e-5)
                          & (times <= segment['offset'] + segment['duration'] + 1e-5)]:
            poses.append((segment['clip'] + '@' + str(float(time)), sample(channels['weapononehand'], float(time)), 'reverse'))
    result = dict(rows=[], max_guard_depth=0., tolerance_units=.05,
                  method='Actual NIF skinning; surface samples; generalized solid-angle winding')
    for label, pose, grip in poses:
        world = vm.fk(pose)
        for side in 'LR':
            profile = profiles[side];anchor = np.asarray(profile['anchor_native_hand']);point = np.asarray(profile['hilt_point'])
            hand = world[f'Bip01 {side} Hand'];frame = world[FRAME_NODES[side]]
            variants = ['forward', 'reverse'] if grip is None else [grip]
            for state in variants:
                if state == 'E':
                    # D ends reverse; the forward requests come from primary MDL events.
                    elapsed = 9.200965881 - e_forward[side]
                    item = delta(side, 'down_to_up', elapsed)
                else:
                    item = delta(side, 'up_to_down' if state == 'reverse' else 'down_to_up')
                rotation = frame[:3, :3] @ item
                mount = to4(rotation, hand[:3, 3] + hand[:3, :3] @ anchor - rotation @ point)
                for (own, name), skin in hands.items():
                    if own != side: continue
                    surface = skin.triangles(world)
                    for weapon, samples in guards.items():
                        if side == 'L': samples = samples * [-1., 1., 1.]
                        row = dict(pose=label, side=side, grip=state, hand=name, weapon=weapon,
                                   **winding_penetration(surface, samples @ mount[:3, :3].T + mount[:3, 3]))
                        result['max_guard_depth'] = max(result['max_guard_depth'], row['max_depth'])
                        result['rows'].append(row)
    result['samples'] = len(result['rows'])
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'rows'}, indent=2))
    raise SystemExit(result['max_guard_depth'] > result['tolerance_units'])


if __name__ == '__main__': main()
