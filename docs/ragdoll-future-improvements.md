# Ragdoll System - Architecture and Future Improvements

## Current Architecture (Post-Refactor)

The ragdoll system now uses Jolt's `SkeletonMapper` for proper synchronization between
the physics skeleton and the animation skeleton. This solves the previous rotation
mismatch issues where the visual mesh would be rotated relative to the physics bodies.

### Key Components

1. **RagdollSettingsBuilder** (`ragdollbuilder.hpp/cpp`)
   - Builds `JPH::RagdollSettings` from an OSG skeleton
   - Defines the physics skeleton hierarchy (20 bones for humanoid biped)
   - Creates collision shapes and joint constraints
   - Pure physics setup - no transform sync logic

2. **RagdollSkeletonMapper** (`skeletonmapper.hpp/cpp`)
   - Wraps Jolt's `SkeletonMapper` for physics-to-animation mapping
   - Builds a mirror animation skeleton from OSG
   - Computes neutral poses at ragdoll creation (bind poses)
   - Uses proper delta-based transforms instead of absolute rotations
   - Handles unmapped joints through interpolation
   - Locks translations to prevent mesh stretching

3. **RagdollWrapper** (`ragdollwrapper.hpp/cpp`)
   - High-level wrapper managing ragdoll lifecycle
   - Delegates transform sync to `RagdollSkeletonMapper`
   - Provides impulse application, body queries, etc.

### How SkeletonMapper Solves the Rotation Issue

The previous approach had these problems:
- Computed absolute rotations for each bone
- Didn't account for bind pose differences
- Accumulated floating-point errors
- Assumed consistent coordinate frames across NIF, OSG, and Jolt

The new approach:
1. Captures bind poses at ragdoll creation for both skeletons
2. Uses Jolt's `SkeletonMapper::Map()` which computes proper delta rotations
3. Interpolates unmapped joints (fingers, toes, equipment bones) correctly
4. Locks translations to prevent stretching when constraints yield

## Short-term Improvements

- [ ] Add debug visualization for physics bodies and constraints
- [ ] Tune joint limits per bone type (profile actual character motion range)
- [ ] Review mass distribution ratios
- [ ] Test with creature skeletons (non-humanoid)
- [ ] Add option for powered ragdoll during hit reactions

## Medium-term Improvements

- [ ] **Data-driven config**: Move ragdoll parameters (mass, limits, shape types) to
      external YAML/JSON files for moddability
- [ ] **Simplified physics skeleton**: Consider fewer physics bones (skip fingers,
      merge spine segments) with mapping back to full skeleton
- [ ] **Partial ragdoll**: Per-bone blend weights (e.g., ragdoll arms while legs animate)
- [ ] **Get-up animations**: Detect when ragdoll comes to rest, blend to get-up animation

## Long-term Improvements

- [ ] **Powered ragdoll**: Use `DriveToPoseUsingMotors()` for hit reactions, stumbling
- [ ] **Ragdoll pooling**: Pre-create settings for common skeletons, instantiate quickly
- [ ] **Cloth simulation**: Add soft-body physics for capes, robes
- [ ] **Dismemberment**: Support for detaching body parts on critical hits

## Physics Skeleton Bones (Humanoid Biped)

```
bip01 pelvis (root)
├── bip01 spine
│   └── bip01 spine1
│       └── bip01 spine2
│           ├── bip01 neck
│           │   └── bip01 head
│           ├── bip01 l clavicle
│           │   └── bip01 l upperarm
│           │       └── bip01 l forearm
│           │           └── bip01 l hand
│           └── bip01 r clavicle
│               └── bip01 r upperarm
│                   └── bip01 r forearm
│                       └── bip01 r hand
├── bip01 l thigh
│   └── bip01 l calf
│       └── bip01 l foot
└── bip01 r thigh
    └── bip01 r calf
        └── bip01 r foot
```

## Technical Notes

### Coordinate Systems
- **NIF files**: Row-major matrices, Y-forward, Z-up
- **OSG**: Column-major matrices, Y-forward, Z-up
- **Jolt**: Column-major, Y-up by default (but we use Z-up configuration)

### Quaternion Conventions
- OSG: `osg::Quat(x, y, z, w)`
- Jolt: `JPH::Quat(x, y, z, w)` - same order, direct conversion works

### Matrix Conversion
NIF to OSG requires transpose due to row/column major difference.
See `components/nif/niftypes.hpp` for the conversion code.
