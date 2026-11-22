#!/usr/bin/env python3
"""Measure the skinned left-hand surface against a native polearm.

Reads emitted KFs independently of the family retarget solver. Samples vertices,
face centres and edge midpoints: vertices alone miss a broad palm triangle
cutting through the shaft.
"""
import argparse
import json
from pathlib import Path
import sys

import numpy as np

from retarget_kf import cross, norm
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'third_party/io_scene_mw/lib'))
from es3 import nif
from audit_slide_pose import distances
from audit_viewmodel_continuity import tracks
from retarget_kf import quat_matrix


def matrix(rotation, translation):
    result = np.eye(4)
    result[:3, :3], result[:3, 3] = rotation, translation
    return result


class Rig:
    def __init__(self, path):
        scene = nif.NiStream(); scene.load(path)
        self.nodes = {node.name: node for node in scene.objects_of_type(nif.NiNode) if node.name}
        self.parents = {child.name: parent.name for parent in self.nodes.values() for child in parent.children
                        if isinstance(child, nif.NiNode) and child.name}

    def world(self, channels, index):
        result = {}
        def get(name):
            if name not in result:
                node = self.nodes[name]
                rotation, translation = np.asarray(node.rotation), np.asarray(node.translation)
                if name in channels:
                    q, p = channels[name]
                    rotation, translation = quat_matrix(q[index, 1:][[1, 2, 3, 0]]), p[index, 1:]
                local = matrix(rotation, translation)
                result[name] = get(self.parents[name])@local if name in self.parents else local
            return result[name]
        for name in self.nodes: get(name)
        return result


class Hand:
    def __init__(self, path):
        scene = nif.NiStream(); scene.load(path)
        self.parts = []
        for shape in scene.objects_of_type(nif.NiTriShape):
            if not shape.name.startswith('Tri Left Hand') or not shape.skin: continue
            self.parts.append((np.asarray(shape.data.vertices, float), np.asarray(shape.data.triangles),
                [(bone.name, data.vertex_weights['f0'], data.vertex_weights['f1'], data.matrix)
                 for bone, data in shape.bone_influences]))
        if not self.parts: raise ValueError(f'No skinned left hand found: {path}')

    def triangles(self, world):
        result = []
        inverse = np.linalg.inv(world['Weapon Bone'])
        for original, faces, influences in self.parts:
            points = np.zeros_like(original)
            for name, indices, weights, bind in influences:
                transform = world[name]@bind
                points[indices] += weights[:, None]*(original[indices]@transform[:3, :3].T+transform[:3, 3])
            points = points@inverse[:3, :3].T+inverse[:3, 3]
            result.append(points[faces])
        return np.concatenate(result)


def weapon_triangles(path):
    scene = nif.NiStream(); scene.load(path)
    result = []
    def visit(node, parent):
        transform = parent@node.matrix
        if isinstance(node, nif.NiTriShape):
            points = np.asarray(node.data.vertices)@transform[:3, :3].T+transform[:3, 3]
            result.append(points[np.asarray(node.data.triangles)])
        for child in getattr(node, 'children', ()):
            if isinstance(child, nif.NiAVObject): visit(child, transform)
    for root in scene.roots: visit(root, np.eye(4))
    return np.concatenate(result)


def surface_samples(triangles):
    return np.concatenate((triangles.reshape(-1, 3), triangles.mean(1),
        (triangles[:, 0]+triangles[:, 1])*.5, (triangles[:, 0]+triangles[:, 2])*.5,
        (triangles[:, 1]+triangles[:, 2])*.5))


def penetration(triangles, points):
    # A non-axial ray avoids the staff's regular ring edges. Exact distances then
    # distinguish a small contact seam from the shaft passing through the palm.
    direction = np.array([1., .187, .357])
    first, second = triangles[:, 1]-triangles[:, 0], triangles[:, 2]-triangles[:, 0]
    h = cross(direction, second)
    determinant = np.einsum('ij,ij->i', first, h)
    inverse = np.divide(1., determinant, out=np.zeros_like(determinant), where=abs(determinant) > 1e-9)
    offset = points[:, None]-triangles[None, :, 0]
    u = np.einsum('pti,ti->pt', offset, h)*inverse
    q = cross(offset, first)
    v = np.einsum('i,pti->pt', direction, q)*inverse
    t = np.einsum('ti,pti->pt', second, q)*inverse
    inside = (((u >= 0) & (v >= 0) & (u+v <= 1) & (t > 1e-9)
               & (abs(determinant) > 1e-9)).sum(1) % 2).astype(bool)
    depths = [float(distances(triangles, point).min()) for point in points[inside]]
    return dict(inside_samples=len(depths), max_depth=max(depths, default=0.))


def finger_arc_enclosure(world):
    """Reject a collision-free hand moved beside the shaft instead of around it."""
    inverse = np.linalg.inv(world['Weapon Bone'])
    centres, enclosed = [], 0
    for number in range(1, 5):
        names = (f'Bip01 L Finger{number}', f'Bip01 L Finger{number}1', f'Bip01 L Finger{number}2')
        points = np.array([(inverse@world[name])[:3, 3][[0, 2]] for name in names])
        a = 2*(points[1:]-points[0])
        b = np.sum(points[1:]**2, axis=1)-np.sum(points[0]**2)
        if abs(np.linalg.det(a)) < 1e-8: continue
        centre = np.linalg.solve(a, b)
        radius = float(norm(points[0]-centre))
        distance = float(norm(centre))
        enclosed += distance < radius
        centres.append(distance)
    return dict(enclosed_finger_arcs=int(enclosed), max_arc_centre_distance=max(centres, default=0.))


def inspect(data, rig, hands, weapons, groups, stride):
    rows = []
    for group in groups:
        channels = tracks(data/'Animations/brutal_fp_staff'/(group+'.kf'))
        times = channels['Weapon Bone'][0][:, 0]
        for index in range(0, len(times), stride):
            world = rig.world(channels, index)
            for hand_name, hand in hands.items():
                surface = surface_samples(hand.triangles(world))
                for weapon_name, weapon in weapons.items():
                    rows.append(dict(group=group, time=float(times[index]), hand=hand_name, weapon=weapon_name,
                                     **penetration(weapon, surface), **finger_arc_enclosure(world)))
    return dict(data=str(data), samples=len(rows), max_depth=max((row['max_depth'] for row in rows), default=0.),
                min_enclosed_finger_arcs=min((row['enclosed_finger_arcs'] for row in rows), default=0),
                worst=sorted(rows, key=lambda row: row['max_depth'], reverse=True)[:20], rows=rows)


def slerp(first, second, weight):
    first, second = first/norm(first), second/norm(second)
    cosine = float(first@second)
    if cosine < 0: second, cosine = -second, -cosine
    cosine = min(1., cosine)
    if cosine > .999999: return ((1-weight)*first+weight*second)/norm((1-weight)*first+weight*second)
    angle = np.arccos(cosine)
    return (np.sin((1-weight)*angle)*first+np.sin(weight*angle)*second)/np.sin(angle)


def sample_channels(channels, time):
    result = {}
    for name, (rotations, translations) in channels.items():
        index = int(np.clip(np.searchsorted(rotations[:, 0], time, side='right')-1, 0, len(rotations)-1))
        following = min(index+1, len(rotations)-1)
        duration = float(rotations[following, 0]-rotations[index, 0])
        weight = float(np.clip((time-rotations[index, 0])/duration, 0, 1)) if duration else 0.
        q = slerp(rotations[index, 1:], rotations[following, 1:], weight)
        p = (1-weight)*translations[index, 1:]+weight*translations[following, 1:]
        result[name] = q, p
    return result


def inspect_blends(data, rig, hands, weapons, stride):
    """Mirror native per-bone shortest-path slerp and linear translation at gait changes."""
    names = ('idle1h', 'runforward1h', 'brutal_sprint1h')
    channels = {name: tracks(data/'Animations/brutal_fp_staff'/(name+'.kf')) for name in names}
    rows = []
    for source, target in zip(names, names[1:]+names[:1]):
        source_times = channels[source]['Weapon Bone'][0][:, 0]
        for index in range(0, len(source_times), stride):
            first = sample_channels(channels[source], float(source_times[index]))
            for weight in (.25, .5, .75):
                second = sample_channels(channels[target], .2*weight)
                blended = {}
                for name, (q1, p1) in second.items():
                    q0, p0 = first[name]
                    q, p = slerp(q0, q1, weight), (1-weight)*p0+weight*p1
                    blended[name] = np.array([[0., *q]]), np.array([[0., *p]])
                world = rig.world(blended, 0)
                enclosure = finger_arc_enclosure(world)
                for hand_name, hand in hands.items():
                    surface = surface_samples(hand.triangles(world))
                    for weapon_name, weapon in weapons.items():
                        rows.append(dict(source=source, target=target, source_time=float(source_times[index]),
                            blend_weight=weight, hand=hand_name, weapon=weapon_name,
                            **penetration(weapon, surface), **enclosure))
    return dict(data=str(data), samples=len(rows), max_depth=max((row['max_depth'] for row in rows), default=0.),
        min_enclosed_finger_arcs=min((row['enclosed_finger_arcs'] for row in rows), default=0),
        worst=sorted(rows, key=lambda row: row['max_depth'], reverse=True)[:20], rows=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('data', type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--skeleton', type=Path, required=True)
    parser.add_argument('--hand-mesh', type=Path, action='append', required=True)
    parser.add_argument('--weapon-mesh', type=Path, action='append', required=True)
    parser.add_argument('--groups', nargs='+', default=['idle1h', 'runforward1h', 'brutal_sprint1h'])
    parser.add_argument('--stride', type=int, default=4)
    parser.add_argument('--blends', action='store_true', help='Also inspect mid-transitions between idle, run and sprint')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.stride < 1: parser.error('--stride must be positive')
    rig = Rig(args.skeleton)
    hands = {path.stem: Hand(path) for path in args.hand_mesh}
    weapons = {path.stem: weapon_triangles(path) for path in args.weapon_mesh}
    report = dict(result=inspect(args.data, rig, hands, weapons, args.groups, args.stride))
    if args.baseline:
        report['baseline'] = inspect(args.baseline, rig, hands, weapons, args.groups, args.stride)
    if args.blends:
        report['result_blends'] = inspect_blends(args.data, rig, hands, weapons, args.stride)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({name: {key: value for key, value in result.items() if key not in ('rows', 'worst')}
                      for name, result in report.items()}, indent=2))


if __name__ == '__main__':
    main()
