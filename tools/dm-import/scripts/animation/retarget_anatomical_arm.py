"""Bind calibration for the anatomical arm retarget.

The source's weighted Wrist helper supplies a distal forearm roll guide. Hand
is the palm flexion pivot. Finger-root layout is measured only in the reference
bind pose: authored per-clip MCP translations must not rotate the rigid palm.
No source or native bind matrix is changed by these functions.
"""
import numpy as np

from retarget_kf import cross, norm

from retarget_contract import CONTRACT


def frame(a, b):
    """Right-handed orthonormal frame; independent of importer construction."""
    a = a / norm(a)
    b = b - a * np.dot(a, b)
    b = b / norm(b)
    return np.column_stack((a, b, cross(a, b)))


def calibrate_arm_frames(source_bind, native_bind, native_local):
    """Return fixed physical palm maps and bind-oriented upper/forearm guides.

    ``source_bind`` and ``native_bind`` contain world matrices. ``native_local``
    maps names to (rotation, translation). Explicit landmark/digit identities
    come from the validated correspondence contract, never approximate names.
    """
    source = {name.casefold(): matrix for name, matrix in source_bind.items()}
    result = {}
    for side, row in CONTRACT['sides'].items():
        src, dst = row['source'], row['target']
        S, E, H = [source[src[role].casefold()] for role in ('shoulder', 'elbow', 'palm')]
        normal = cross(E[:3, 3] - S[:3, 3], H[:3, 3] - E[:3, 3])
        normal /= norm(normal)
        nS, nE, nH = [native_bind[dst[role]] for role in ('shoulder', 'elbow', 'palm')]
        native_normal = cross(nE[:3, 3] - nS[:3, 3], nH[:3, 3] - nE[:3, 3])
        native_normal /= norm(native_normal)
        digits = [d for d in row['digits'] if d['role'] != 'thumb']
        source_mcp = np.asarray([source[d['source'][0].casefold()][:3, 3] for d in digits])
        native_mcp = np.asarray([native_local[d['target'][0]][1] for d in digits])
        source_palm = frame(source_mcp.mean(0) - H[:3, 3], source_mcp[-1] - source_mcp[0])
        native_palm = frame(native_mcp.mean(0), native_mcp[-1] - native_mcp[0])
        result[side] = dict(
            hand_map=H[:3, :3].T @ source_palm @ native_palm.T,
            native_mcp_mean=native_mcp.mean(0),
            source_mcp_reference_hand_local=H[:3, :3].T @ (source_mcp.mean(0) - H[:3, 3]),
            upper_basis=frame(native_local[dst['elbow']][1], nS[:3, :3].T @ native_normal),
            fore_basis=frame(native_local[dst['palm']][1], nE[:3, :3].T @ native_normal),
            upper_guide=S[:3, :3].T @ normal,
            distal_guide=source[src['distal_twist_helper'].casefold()][:3, :3].T @ normal)
    return result


def mcp_hand_target(source_world, side, calibration, basis, to_native, reference=False):
    """Align the native MCP-row centroid, retaining the fixed rigid Hand frame.

    This measured proportion adaptation changes the arm anchor, never the Hand
    rotation. ``reference`` uses a rigid bind MCP landmark instead of authored
    per-clip MCP-root translations. Both choices are explicit and auditable.
    """
    source = {name.casefold(): matrix for name, matrix in source_world.items()}
    row = CONTRACT['sides'][side]
    hand = source[row['source']['palm'].casefold()]
    if reference:
        centre = hand[:3, 3] + hand[:3, :3] @ calibration['source_mcp_reference_hand_local']
    else:
        digits = [digit for digit in row['digits'] if digit['role'] != 'thumb']
        centre = np.mean([source[digit['source'][0].casefold()][:3, 3] for digit in digits], axis=0)
    rotation = basis @ hand[:3, :3] @ calibration['hand_map']
    return to_native(centre) - rotation @ calibration['native_mcp_mean']


def solve_arm_targets(pose, source_world, native_world, native_local, native_parents,
                      calibration, basis, to_native, hand_targets=None):
    """Solve both anatomical arm targets while retaining the native skin binds.

    Source Hand anchors the palm, source Shoulder/Elbow/Hand define articulation,
    and the weighted distal Wrist supplies pronation only. Native bone lengths
    stay fixed; the clavicle translation absorbs the proportion difference.
    ``hand_targets`` optionally supplies measured native-world palm-root anchors
    per side, for an explicit MCP/physical contact adaptation. It translates the
    coherent arm chain instead of changing its articulation. Items, fingers,
    cameras and animation clocks are outside this pure kernel.
    """
    source = {name.casefold(): matrix for name, matrix in source_world.items()}
    result = dict(pose)
    for side, values in calibration.items():
        row = CONTRACT['sides'][side]
        src, dst = row['source'], row['target']
        S, E, H = [source[src[role].casefold()] for role in ('shoulder', 'elbow', 'palm')]
        upper = basis @ (E[:3, 3] - S[:3, 3])
        fore = basis @ (H[:3, 3] - E[:3, 3])
        upper /= norm(upper)
        fore /= norm(fore)
        hand = (np.asarray(hand_targets[side], float) if hand_targets and side in hand_targets
                else to_native(H[:3, 3]))
        if hand.shape != (3,) or not np.isfinite(hand).all():
            raise ValueError(f'Invalid native-world palm anchor for {side}')
        elbow = hand - norm(native_local[dst['palm']][1]) * fore
        shoulder = elbow - norm(native_local[dst['elbow']][1]) * upper
        Ru = frame(upper, basis @ S[:3, :3] @ values['upper_guide']) @ values['upper_basis'].T
        distal = source[src['distal_twist_helper'].casefold()]
        Rf = frame(fore, basis @ distal[:3, :3] @ values['distal_guide']) @ values['fore_basis'].T
        Rh = basis @ H[:3, :3] @ values['hand_map']
        clavicle = dst['clavicle']
        parent = native_world[native_parents[clavicle]]
        Rc = native_local[clavicle][0]
        Rcw = parent[:3, :3] @ Rc
        position = shoulder - Rcw @ native_local[dst['shoulder']][1]
        result[clavicle] = Rc, parent[:3, :3].T @ (position - parent[:3, 3])
        result[dst['shoulder']] = Rcw.T @ Ru, native_local[dst['shoulder']][1]
        result[dst['elbow']] = Ru.T @ Rf, native_local[dst['elbow']][1]
        result[dst['palm']] = Rf.T @ Rh, native_local[dst['palm']][1]
    return result


def follow_finger_directions(pose, source_world, native_world, native_local, native_parents, basis, side):
    """Controlled three-phalange direction transfer; not a full skin-frame solve.

    The explicit contract supplies four source landmarks per digit. Native MCP
    positions and lengths stay intact. Source fourth-node skin rotation is not
    transferred by this reduction; callers must declare that limitation.
    """
    result = dict(pose)
    source = {name.casefold(): matrix for name, matrix in source_world.items()}
    unit = lambda value: value / norm(value)
    for digit in CONTRACT['sides'][side]['digits']:
        points = np.asarray([source[name.casefold()][:3, 3] for name in digit['source']])
        directions = np.diff(points, axis=0) @ basis.T
        chain = digit['target']
        parent = native_world[native_parents[chain[0]]][:3, :3]
        for index, node in enumerate(chain):
            rotation, translation = result.get(node, native_local[node])
            current = parent @ rotation
            along = native_local[chain[index + 1]][1] if index + 1 < len(chain) else native_local[node][1]
            a, b = unit(current @ along), unit(directions[index])
            turn = cross(a, b); sine, cosine = norm(turn), float(a @ b)
            if sine < 1e-7 and cosine < 0:
                raise ValueError(f'Ambiguous antipodal finger direction: {side}/{digit["role"]}/{index}')
            if sine > 1e-7:
                x, y, z = turn / sine
                skew = np.array([[0., -z, y], [z, 0., -x], [-y, x, 0.]])
                current = (np.eye(3) + sine * skew + (1 - cosine) * skew @ skew) @ current
            result[node] = parent.T @ current, translation
            parent = current
    return result
