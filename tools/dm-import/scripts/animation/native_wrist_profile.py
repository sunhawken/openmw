"""Native wrist pose preference around the geometric MCP palm normal.

The MCP basis is an animation landmark frame, not a clinical skin-crease zero.
The C1 side bound keeps ordinary game poses from folding the palm sideways.
"""
import numpy as np


from retarget_kf import norm
def soft_bound(value, limit=30., halfwidth=5.):
    magnitude = abs(value)
    low, high = limit - halfwidth, limit + halfwidth
    if magnitude <= low:
        return float(value)
    if magnitude >= high:
        return float(np.copysign(limit, value))
    amount = magnitude - low
    return float(np.copysign(low + amount - amount * amount / (4 * halfwidth), value))


def lateral_cap(rotation, translation, palm, limit=30., halfwidth=5.):
    """Return Hand-local correction; local fingers and held item stay rigid."""
    direction = translation / norm(translation)
    a, b, c = (rotation @ palm).T @ direction
    angle = float(np.degrees(np.arctan2(b, a)))
    amount = angle - soft_bound(angle, limit, halfwidth)
    axis = palm[:, 2] / norm(palm[:, 2])
    x, y, z = axis
    skew = np.array([[0., -z, y], [z, 0., -x], [-y, x, 0.]])
    radians = np.radians(amount)
    correction = np.eye(3) + np.sin(radians) * skew + (1 - np.cos(radians)) * skew @ skew
    return correction, amount
