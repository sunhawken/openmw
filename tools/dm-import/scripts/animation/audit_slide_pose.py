#!/usr/bin/env python3
"""Re-read slide KFs and check the starter clothing geometry against them.

--meshes is an extraction of Morrowind's common shirt01, pants00 and leather00
shoes from the user's game. Rigid part attachment and left-side reflection match
SceneUtil::attach; the shirt is evaluated with its NiSkinData inverse binds and
weights.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from slide_pose import Rig, nif, rotation


PARTS = [('c_m_shirt_c_common01.nif', 'Chest'), ('c_m_pants_g_common00.nif', 'Groin')]
for side in ('Left', 'Right'):
    PARTS += [(f'c_m_pants_{part}_common00.nif', side+' '+bone)
              for part, bone in (('ul', 'Upper Leg'), ('k', 'Knee'), ('a', 'Ankle'))]
    PARTS += [('c_m_shoe_f_leather00.nif', side+' Foot')]


class Clothing:
    def __init__(self, folder):
        self.parts = []
        for filename, attachment in PARTS:
            path = next(folder.rglob(filename))
            stream = nif.NiStream(); stream.load(path)
            def visit(node, parent):
                matrix = parent @ node.matrix
                if isinstance(node, nif.NiTriShape):
                    self.parts.append((attachment, node, matrix))
                for child in node.children:
                    if isinstance(child, nif.NiAVObject): visit(child, matrix)
            visit(stream.root, np.eye(4))

    def triangles(self, world):
        triangles, labels = [], []
        for attachment, shape, matrix in self.parts:
            vertices = np.asarray(shape.data.vertices, float)
            if shape.skin:
                result = np.zeros_like(vertices)
                for bone, data in shape.bone_influences:
                    indices, weights = data.vertex_weights['f0'], data.vertex_weights['f1']
                    transform = world[bone.name] @ data.matrix
                    result[indices] += weights[:, None]*(vertices[indices]@transform[:3, :3].T+transform[:3, 3])
            else:
                reflect = np.diag([-1., 1., 1., 1.]) if attachment.startswith('Left') else np.eye(4)
                transform = world[attachment] @ reflect @ matrix
                result = vertices@transform[:3, :3].T+transform[:3, 3]
            faces = result[shape.data.triangles]
            triangles.extend(faces)
            labels.extend([attachment]*len(faces))
        return np.asarray(triangles), np.asarray(labels)


def distances(triangles, point):
    """Exact distance to each triangle, including its edges and vertices."""
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    ab, ac = b-a, c-a
    normal = np.cross(ab, ac)
    normal /= np.maximum(np.linalg.norm(normal, axis=1)[:, None], 1e-15)
    height = np.einsum('ij,ij->i', point-a, normal)
    projected = point-height[:, None]*normal
    v = projected-a
    aa, bb, cc = (np.einsum('ij,ij->i', x, y) for x, y in ((ab, ab), (ab, ac), (ac, ac)))
    av, cv = (np.einsum('ij,ij->i', x, v) for x in (ab, ac))
    det = aa*cc-bb*bb
    s, t = (cc*av-bb*cv)/np.maximum(det, 1e-15), (aa*cv-bb*av)/np.maximum(det, 1e-15)
    inside = (s >= 0) & (t >= 0) & (s+t <= 1) & (det > 1e-12)
    d = np.where(inside, abs(height), np.inf)
    for start, end in ((a, b), (b, c), (c, a)):
        edge = end-start
        f = np.clip(np.einsum('ij,ij->i', point-start, edge)/np.maximum(np.einsum('ij,ij->i', edge, edge), 1e-15), 0, 1)
        d = np.minimum(d, np.linalg.norm(start+f[:, None]*edge-point, axis=1))
    return d


def leg_intersections(triangles, labels):
    """Find noncoplanar surface crossings between the left/right clothing.

    Broad-phase triangle bounds, then segment/triangle intersection in both
    directions. Tests the real rigid parts, not overlapping screen silhouettes.
    """
    left = triangles[np.char.startswith(labels, 'Left ')]
    right = triangles[np.char.startswith(labels, 'Right ')]
    pairs = np.all(left.min(1)[:, None] <= right.max(1)[None, :]+1e-8, axis=2)
    pairs &= np.all(left.max(1)[:, None] >= right.min(1)[None, :]-1e-8, axis=2)
    li, ri = np.nonzero(pairs)
    a, b = left[li], right[ri]
    crossings = np.zeros(len(li), bool)
    def intersects(start, end, tri):
        direction, e1, e2 = end-start, tri[:, 1]-tri[:, 0], tri[:, 2]-tri[:, 0]
        h = np.cross(direction, e2)
        det = np.einsum('ij,ij->i', e1, h)
        valid = abs(det) > 1e-9
        inv = np.divide(1., det, out=np.zeros_like(det), where=valid)
        offset = start-tri[:, 0]
        u = inv*np.einsum('ij,ij->i', offset, h)
        q = np.cross(offset, e1)
        v = inv*np.einsum('ij,ij->i', direction, q)
        t = inv*np.einsum('ij,ij->i', e2, q)
        return valid & (u >= -1e-8) & (v >= -1e-8) & (u+v <= 1+1e-8) & (t >= -1e-8) & (t <= 1+1e-8)
    for edge in range(3):
        crossings |= intersects(a[:, edge], a[:, (edge+1) % 3], b)
        crossings |= intersects(b[:, edge], b[:, (edge+1) % 3], a)
    return int(crossings.sum())


def camera_weight(t, marks):
    if t <= marks[1][0]: return t/marks[1][0]
    if t <= marks[4][0]: return 1.
    return max(0., 1-(t-marks[4][0])/(marks[5][0]-marks[4][0]))


def draw_view(triangles, labels, eye, view, size=(480, 300), pitch=0.):
    image = Image.new('RGB', size, '#d8e0e7'); draw = ImageDraw.Draw(image)
    width, height = size
    if view == 'eye':
        points = (triangles-eye) @ rotation([1, 0, 0], pitch).T
        valid = np.all(points[:, :, 1] > 1, axis=1)
        f = height/(2*np.tan(np.radians(69/2)))
        uv = np.stack((width/2+f*points[:, :, 0]/np.maximum(points[:, :, 1], .01),
                       height/2-f*points[:, :, 2]/np.maximum(points[:, :, 1], .01)), axis=2)
        order = np.argsort(points[:, :, 1].mean(axis=1))[::-1]
        draw.line((0, height/2, width, height/2), fill='#9aa6b2')
    else:
        horizontal = 1 if view == 'side' else 0
        center = 25 if view == 'side' else 0
        uv = np.stack((width/2+(triangles[:, :, horizontal]-center)*2,
                       height-15-triangles[:, :, 2]*2), axis=2)
        valid = np.ones(len(triangles), bool)
        order = np.argsort(triangles[:, :, 0 if view == 'side' else 1].mean(axis=1))
        draw.line((0, height-15, width, height-15), fill='#7c8b79', width=2)
        e = [width/2+(eye[horizontal]-center)*2, height-15-eye[2]*2]
        draw.ellipse((e[0]-6, e[1]-6, e[0]+6, e[1]+6), outline='red', width=2)
    mask = Image.new('1', size); painter = ImageDraw.Draw(mask)
    for index in order:
        if not valid[index]: continue
        color = '#bc8b50' if labels[index] == 'Chest' else '#514b43' if 'Foot' in labels[index] else '#687c90'
        polygon = [tuple(p) for p in np.clip(uv[index], -10000, 10000)]
        draw.polygon(polygon, fill=color, outline='#35424b')
        painter.polygon(polygon, fill=1)
    return image, float(np.asarray(mask).mean())


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--skeleton', type=Path, required=True)
    p.add_argument('--arms-skeleton', type=Path, required=True,
                   help='Actual rigid first-person rig supplies the Camera and pitch pivot')
    p.add_argument('--meshes', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--eye-drop', type=float, default=70.)
    p.add_argument('--eye-radius', type=float, default=3.)
    p.add_argument('--pitches', type=float, nargs='+', default=[0., 20., 30., 40.])
    a = p.parse_args()
    rig, clothing = Rig(a.skeleton), Clothing(a.meshes)
    manifest = json.loads((a.data/'dishonored-slide-import.json').read_text())
    marks = manifest['markers']; path = a.data/'Animations/brutal_firstperson/brutal_slide.kf'
    arms = Rig(a.arms_skeleton)
    arm_path = a.data/'Animations/brutal_fp/brutal_slide.kf'
    arm_world = arms.world(arms.sample(arm_path, 0.))
    base_eye = arm_world['Camera'][:3, 3]
    # Match RotateController: pitch about the imported fixed pivot plus the
    # carrier translation. Native sneak/slide offset remains outside pitch.
    pivot = arm_world['Brutal Camera Pivot'][:3, 3] + arm_world['Brutal Camera Carrier'][:3, 3]
    def eye_at(t, pitch):
        return pivot + rotation([1, 0, 0], -pitch) @ (base_eye-pivot) - np.array([0., 0., a.eye_drop*camera_weight(t, marks)])
    end = marks[-1][0]
    ground = {'z': float('inf')}
    crossing = {'triangle_pairs': 0, 'time': 0.}
    metrics = {pitch: {'eye_surface_gap': {'gap': float('inf')}, 'conservative_interval_gap': float('inf')}
               for pitch in a.pitches}
    previous = {}
    samples = []
    # Dense samples re-read the KF (including between exported 30 Hz keys).
    for t in np.linspace(0., end, int(np.ceil(end*600))+1):
        triangles, labels = clothing.triangles(rig.world(rig.sample(path, t)))
        z = triangles[:, :, 2].min()
        if z < ground['z']: ground = dict(z=float(z), time=float(t))
        count = leg_intersections(triangles, labels)
        if count > crossing['triangle_pairs']: crossing = dict(triangle_pairs=count, time=float(t))
        for pitch in a.pitches:
            eye = eye_at(t, pitch)
            d = distances(triangles, eye)-a.eye_radius
            index = int(np.argmin(d)); result = metrics[pitch]
            if d[index] < result['eye_surface_gap']['gap']:
                result['eye_surface_gap'] = dict(gap=float(d[index]), time=float(t), part=str(labels[index]), pitch=pitch)
            relative = triangles-eye
            if pitch in previous:
                # Conservative interval bound for linearly moving vertices:
                # no surface can approach faster than the fastest vertex.
                speed_bound = np.linalg.norm(relative-previous[pitch][0], axis=2).max()
                result['conservative_interval_gap'] = min(result['conservative_interval_gap'],
                    float(min(d.min(), previous[pitch][1])-speed_bound))
            previous[pitch] = relative, d.min()
    worst = min((v['eye_surface_gap'] for v in metrics.values()), key=lambda v: v['gap'])
    swept_gap = min(v['conservative_interval_gap'] for v in metrics.values())
    times = [0., marks[1][0]*.5, marks[2][0]+.2, marks[4][0]+(end-marks[4][0])*.5, end]
    sheet = Image.new('RGB', (480*5, 330*4), 'white')
    for column, t in enumerate(times):
        world = rig.world(rig.sample(path, t)); triangles, labels = clothing.triangles(world)
        eye = eye_at(t, 0.)
        row_report = dict(time=t, eye=eye.tolist(), pelvis=world['Bip01 Pelvis'][:3, 3].tolist(),
                          knees={s: world[f'Bip01 {s} Calf'][:3, 3].tolist() for s in 'LR'},
                          hips={s: world[f'Bip01 {s} Thigh'][:3, 3].tolist() for s in 'LR'},
                          ankles={s: world[f'Bip01 {s} Foot'][:3, 3].tolist() for s in 'LR'},
                          leg_surface_crossings=leg_intersections(triangles, labels))
        row_report['knee_flexion_degrees'] = {}
        for side in 'LR':
            hip, knee, ankle = [world[f'Bip01 {side} {name}'][:3, 3] for name in ('Thigh', 'Calf', 'Foot')]
            a1, a2 = knee-hip, ankle-knee
            row_report['knee_flexion_degrees'][side] = float(np.degrees(np.arccos(np.clip(a1@a2/np.linalg.norm(a1)/np.linalg.norm(a2), -1, 1))))
        row_report['part_floor_heights'] = {str(part): float(triangles[labels == part, :, 2].min())
                                           for part in dict.fromkeys(labels)}
        row_report['screen_fraction_by_pitch'] = {}
        for row, (view, pitch) in enumerate((('front', 0.), ('side', 0.), ('eye', 0.), ('eye', 30.))):
            view_eye = eye_at(t, pitch)
            image, coverage = draw_view(triangles, labels, view_eye, view, pitch=pitch)
            sheet.paste(image, (480*column, 330*row+30))
            ImageDraw.Draw(sheet).text((480*column+10, 330*row+8), f'{view} {pitch:g} deg  t={t:.3f}s  eye={view_eye[2]:.1f}', fill='black')
            if view == 'eye':
                row_report['screen_fraction_by_pitch'][pitch] = coverage
                if pitch == 0: row_report['body_screen_fraction'] = coverage
        samples.append(row_report)
    a.output.mkdir(parents=True, exist_ok=True)
    sheet.save(a.output/'pose-contact-sheet.png')
    joins = {}
    for name, start, stop in [('idle_return', 0., end), ('in_loop', marks[1][0], marks[2][0]),
                              ('loop_out', marks[3][0], marks[4][0]), ('loop_seam', marks[2][0], marks[3][0])]:
        x, y = [rig.world(rig.sample(path, time)) for time in (start, stop)]
        joins[name] = dict(max_position_error=max(float(np.linalg.norm(x[n][:3, 3]-y[n][:3, 3])) for n in x),
                           max_rotation_matrix_error=max(float(np.abs(x[n][:3, :3]-y[n][:3, :3]).max()) for n in x))
    errors = []
    if swept_gap < 0: errors.append('eye sphere intersects or approaches the swept clothing surface')
    if ground['z'] < -.5: errors.append('clothing passes below the floor by more than the idle tolerance')
    if crossing['triangle_pairs']: errors.append('left and right leg clothing surfaces cross')
    if any(join['max_position_error'] > .001 or join['max_rotation_matrix_error'] > .001 for join in joins.values()):
        errors.append('body animation phase boundary is discontinuous')
    report = dict(surface='actual owner starter shirt, pants and shoes; rigid attachments and skin weights',
                  camera_baseline=base_eye.tolist(), camera_pitch_pivot=pivot.tolist(), per_pitch=metrics,
                  eye_radius=a.eye_radius, eye_surface_gap=worst, conservative_interval_gap=float(swept_gap),
                  sweep_sample_hz=600, sweep_model='Piecewise linear relative triangle sweep between 600 Hz samples of the written KF.',
                  lowest_vertex=ground, leg_surface_crossings=crossing, joins=joins, samples=samples, errors=errors,
                  limitation='Imported Camera and pitch pivot with linear native slide blend; screen area is projected clothing silhouette, not pixel segmentation. Game capture remains the visual oracle.')
    (a.output/'pose-audit.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))
    return bool(errors)


if __name__ == '__main__': raise SystemExit(main())
