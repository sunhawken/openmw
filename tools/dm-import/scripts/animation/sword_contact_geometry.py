"""Exact triangle contact helpers for native mesh retarget checks.

The functions operate on the rendered mesh triangles. They do not substitute
arm capsules, bone centres or a projected screen silhouette for physical skin.
"""
import numpy as np


from retarget_kf import cross, norm
def close_open_boundaries(triangles, weld=.0001):
    """Close only measured boundary rings for volume classification.

    Rendered triangles remain unchanged. Each manifold boundary ring gets an
    oppositely oriented fan to its own centroid. Nonmanifold/open-chain topology
    fails explicitly; there is no convex hull or capsule substitute.
    """
    points = triangles.reshape(-1, 3)
    _, inverse = np.unique(np.rint(points / weld).astype(np.int64), axis=0, return_inverse=True)
    faces = inverse.reshape(-1, 3)
    positions = np.zeros((int(inverse.max()) + 1, 3))
    counts = np.bincount(inverse)
    np.add.at(positions, inverse, points)
    positions /= counts[:, None]
    edges = {}
    for face in faces:
        for index in range(3):
            edge = (int(face[index]), int(face[(index + 1) % 3]))
            edges.setdefault(tuple(sorted(edge)), []).append(edge)
    if any(len(values) > 2 for values in edges.values()):
        raise ValueError('Nonmanifold skin edge; cannot certify its interior volume')
    boundary = [values[0] for values in edges.values() if len(values) == 1]
    adjacent = {}
    for first, second in boundary:
        adjacent.setdefault(first, set()).add(second)
        adjacent.setdefault(second, set()).add(first)
    if any(len(values) != 2 for values in adjacent.values()):
        raise ValueError('Skin boundary is not a closed manifold ring')
    seen, caps, rings = set(), [], []
    for vertex in adjacent:
        if vertex in seen:
            continue
        pending, component = [vertex], []
        while pending:
            item = pending.pop()
            if item in seen:
                continue
            seen.add(item); component.append(item)
            pending.extend(adjacent[item] - seen)
        centre = positions[component].mean(0)
        members = set(component)
        own_edges = [(a, b) for a, b in boundary if a in members]
        caps.extend(np.asarray([positions[b], positions[a], centre]) for a, b in own_edges)
        rings.append(dict(vertices=len(component), centre=centre.tolist()))
    return (np.concatenate((triangles, np.asarray(caps))) if caps else triangles.copy()), rings


def interior_points(volume, points):
    """Generalized solid-angle winding on a closed actual mesh volume."""
    mask = np.all(points >= volume.min((0, 1)) - 1e-8, axis=1)
    mask &= np.all(points <= volume.max((0, 1)) + 1e-8, axis=1)
    result = np.zeros(len(points), bool)
    indices = np.flatnonzero(mask)
    for chunk in np.array_split(indices, max(1, int(np.ceil(len(indices) / 256)))):
        if not len(chunk):
            continue
        a, b, c = (volume[None, :, index] - points[chunk, None] for index in range(3))
        na, nb, nc = (norm(value, axis=2) for value in (a, b, c))
        numerator = np.einsum('pti,pti->pt', a, cross(b, c))
        denominator = (na * nb * nc + np.einsum('pti,pti->pt', a, b) * nc
                       + np.einsum('pti,pti->pt', b, c) * na + np.einsum('pti,pti->pt', c, a) * nb)
        result[chunk] = np.abs(np.sum(2 * np.arctan2(numerator, denominator), axis=1)) > 2 * np.pi
    return result


def _segment_triangle(start, end, triangle):
    direction = end - start
    first, second = triangle[:, 1] - triangle[:, 0], triangle[:, 2] - triangle[:, 0]
    h = cross(direction, second)
    determinant = np.einsum('ij,ij->i', first, h)
    valid = abs(determinant) > 1e-9
    inverse = np.divide(1., determinant, out=np.zeros_like(determinant), where=valid)
    offset = start - triangle[:, 0]
    u = inverse * np.einsum('ij,ij->i', offset, h)
    q = cross(offset, first)
    v = inverse * np.einsum('ij,ij->i', direction, q)
    t = inverse * np.einsum('ij,ij->i', second, q)
    return valid & (u >= -1e-8) & (v >= -1e-8) & (u + v <= 1 + 1e-8) & (t >= -1e-8) & (t <= 1 + 1e-8)


def _point_triangle(point, triangle):
    a, b, c = triangle[:, 0], triangle[:, 1], triangle[:, 2]
    ab, ac = b - a, c - a
    normal = cross(ab, ac)
    normal /= np.maximum(norm(normal, axis=1)[:, None], 1e-15)
    height = np.einsum('ij,ij->i', point - a, normal)
    offset = point - height[:, None] * normal - a
    aa, bb, cc = [np.einsum('ij,ij->i', x, y) for x, y in ((ab, ab), (ab, ac), (ac, ac))]
    av, cv = [np.einsum('ij,ij->i', x, offset) for x in (ab, ac)]
    determinant = aa * cc - bb * bb
    s = (cc * av - bb * cv) / np.maximum(determinant, 1e-15)
    t = (aa * cv - bb * av) / np.maximum(determinant, 1e-15)
    inside = (s >= 0) & (t >= 0) & (s + t <= 1) & (determinant > 1e-12)
    result = np.where(inside, abs(height), np.inf)
    for start, end in ((a, b), (b, c), (c, a)):
        edge = end - start
        amount = np.clip(np.einsum('ij,ij->i', point - start, edge)
                         / np.maximum(np.einsum('ij,ij->i', edge, edge), 1e-15), 0, 1)
        result = np.minimum(result, norm(start + amount[:, None] * edge - point, axis=1))
    return result


def _edge_edge(a, b, c, d):
    """Minimum segment distance, including parallel and endpoint cases."""
    u, v, w = b - a, d - c, a - c
    aa, bb, cc, dd, ee = [np.einsum('ij,ij->i', x, y)
                         for x, y in ((u, u), (u, v), (v, v), (u, w), (v, w))]
    determinant = aa * cc - bb * bb
    s = np.divide(bb * ee - cc * dd, determinant, out=np.zeros_like(aa), where=determinant > 1e-14)
    t = np.divide(aa * ee - bb * dd, determinant, out=np.zeros_like(aa), where=determinant > 1e-14)
    interior = (determinant > 1e-14) & (s >= 0) & (s <= 1) & (t >= 0) & (t <= 1)
    result = np.where(interior, norm(w + s[:, None] * u - t[:, None] * v, axis=1), np.inf)
    for point, start, end in ((a, c, d), (b, c, d), (c, a, b), (d, a, b)):
        edge = end - start
        amount = np.clip(np.einsum('ij,ij->i', point - start, edge)
                         / np.maximum(np.einsum('ij,ij->i', edge, edge), 1e-15), 0, 1)
        result = np.minimum(result, norm(start + amount[:, None] * edge - point, axis=1))
    return result


def contact(first, second, clearance=.05):
    """Return surface crossings and clearance, using an exact triangle broad phase.

    ``minimum_distance`` is capped at the requested clearance when no nearby
    pair exists. It is a separation predicate, not a penetration-depth estimate.
    """
    pairs = np.all(first.min(1)[:, None] <= second.max(1)[None] + clearance, axis=2)
    pairs &= np.all(first.max(1)[:, None] >= second.min(1)[None] - clearance, axis=2)
    fi, si = np.nonzero(pairs)
    if not len(fi):
        return dict(intersections=0, minimum_distance=float(clearance), clear=True)
    a, b = first[fi], second[si]
    crossings = np.zeros(len(fi), bool)
    for i in range(3):
        crossings |= _segment_triangle(a[:, i], a[:, (i + 1) % 3], b)
        crossings |= _segment_triangle(b[:, i], b[:, (i + 1) % 3], a)
    if crossings.any():
        return dict(intersections=int(crossings.sum()), minimum_distance=0., clear=False)
    minimum = np.full(len(fi), np.inf)
    for i in range(3):
        minimum = np.minimum(minimum, _point_triangle(a[:, i], b))
        minimum = np.minimum(minimum, _point_triangle(b[:, i], a))
        for j in range(3):
            minimum = np.minimum(minimum, _edge_edge(a[:, i], a[:, (i + 1) % 3], b[:, j], b[:, (j + 1) % 3]))
    distance = float(minimum.min())
    return dict(intersections=0, minimum_distance=min(distance, float(clearance)), clear=distance >= clearance - 1e-8)
