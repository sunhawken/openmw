# Next Agent Task: Fix Buoyancy in DynamicObject

## Problem
Objects float 15+ units above water and bounce/oscillate. See `docs/jolt-integration-analysis.md` Part 8.

## Root Cause
In `apps/openmw/mwphysics/dynamicobject.cpp:updateBuoyancy()`:
1. `halfHeight` calculation uses local shape bounds (wrong for rotated shapes) with 5.0 minimum (too large)
2. `buoyancyStrength = 5.0` is 5x too strong

## Fix Required
Rewrite `updateBuoyancy()` to:
1. Get world-space AABB from body (not local shape bounds)
2. Use smaller minimum halfHeight (~0.5)
3. Use realistic buoyancy: force ≈ `mass * gravity` when fully submerged (Archimedes)
4. Increase damping to prevent oscillation

## Key Files
- `apps/openmw/mwphysics/dynamicobject.cpp` lines 312-425
- `docs/jolt-integration-analysis.md` Part 8 for full analysis

## Test
Drop `misc_com_wood_bowl_04` in water. Should float at water surface (z ≈ waterHeight), not 15+ units above.
