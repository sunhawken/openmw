"""Bind-pose bones and vertices of an actor NIF, for planning ragdoll parts offline."""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'third_party/io_scene_mw/lib'))
from es3 import nif  # noqa: E402

IGNORED_SHAPES = ('shadow', 'tri shadow', 'bounding box')


def local_matrix(obj):
    matrix = np.identity(4)
    matrix[:3, :3] = np.asarray(obj.rotation, float) * float(obj.scale)
    matrix[:3, 3] = np.asarray(obj.translation, float)
    return matrix


def transformed(matrix, points):
    return points @ matrix[:3, :3].T + matrix[:3, 3]


class Skeleton:
    """Nodes with parent links and world matrices, plus each vertex's dominant bone."""

    def __init__(self, stream):
        self.parent, self.world, self.order = {}, {}, []
        self.vertices = {}
        self.skin_bones = set()
        shapes = []
        for root in stream.roots_of_type(nif.NiAVObject):
            self._walk(root, None, np.identity(4), shapes)
        for shape, owner, world in shapes:
            self._collect(shape, owner, world)
        self.vertices = {bone: np.concatenate(parts) for bone, parts in self.vertices.items()}

    def _walk(self, obj, parent, world, shapes):
        if obj is None:
            return
        matrix = world @ local_matrix(obj)
        if isinstance(obj, nif.NiNode):
            name = obj.name or parent
            if obj.name and obj.name not in self.parent:
                self.parent[obj.name] = parent
                self.world[obj.name] = matrix
                self.order.append(obj.name)
            for child in obj.children:
                self._walk(child, name, matrix, shapes)
        elif isinstance(obj, nif.NiTriShape):
            lowered = (obj.name or '').lower()
            hidden = bool(int(obj.flags) & 1)
            if not hidden and not lowered.startswith(IGNORED_SHAPES) and parent:
                shapes.append((obj, parent, matrix))

    def _collect(self, shape, owner, world):
        points = np.asarray(shape.data.vertices, float)
        if not len(points):
            return
        skin = shape.skin
        if skin is None or skin.data is None:
            self.vertices.setdefault(owner, []).append(transformed(world, points))
            return
        best = np.full(len(points), -1.0)
        bones = np.full(len(points), -1, int)
        placed = np.zeros_like(points)
        names = []
        for index, (bone, data) in enumerate(zip(skin.bones, skin.data.bone_data)):
            name = bone.name if bone is not None else None
            names.append(name)
            if not name or name not in self.world or not len(data.vertex_weights):
                continue
            self.skin_bones.add(name)
            ids = data.vertex_weights['f0']
            weights = data.vertex_weights['f1']
            matrix = self.world[name] @ local_matrix(data)
            positions = transformed(matrix, points[ids])
            better = weights > best[ids]
            chosen = ids[better]
            best[chosen] = weights[better]
            bones[chosen] = index
            placed[chosen] = positions[better]
        for index, name in enumerate(names):
            mask = bones == index
            if name and mask.any():
                self.vertices.setdefault(name, []).append(placed[mask])

    def extent(self):
        points = np.concatenate(list(self.vertices.values())) if self.vertices else np.zeros((1, 3))
        return points.min(0), points.max(0)
