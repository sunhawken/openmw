# Jolt Physics Implementation for OpenMW

This document describes the Jolt Physics integration that replaces the previous Bullet physics engine, implementing Oblivion-style physics with buoyancy and WIP ragdoll support.

## Overview

The implementation spans ~10,000 lines across 39 files in `apps/openmw/mwphysics/`. Key features:
- Full Jolt Physics integration with proper layer/filter system
- Dynamic objects with realistic physics (items can be pushed, thrown, knocked around)
- Buoyancy system using Jolt's built-in `ApplyBuoyancyImpulse`
- WIP ragdoll system using `JPH::RagdollSettings`
- Object grabbing (Oblivion/Skyrim style telekinesis)

## Architecture

### Layer System

**Object Layers** (`joltlayers.hpp`):
```
WORLD        (1<<0)  - Static geometry (floors, walls)
DOOR         (1<<1)  - Animated doors
ACTOR        (1<<2)  - NPCs, creatures, player
HEIGHTMAP    (1<<3)  - Terrain
PROJECTILE   (1<<4)  - Arrows, spells
WATER        (1<<5)  - Water bodies (for water-walking detection)
CAMERA_ONLY  (1<<6)  - Camera collision only
VISUAL_ONLY  (1<<7)  - Visual collision only
DYNAMIC_WORLD(1<<8)  - Dynamic objects (items that can be pushed)
DEBRIS       (1<<9)  - Corpses, effects (only collides with static)
SENSOR       (1<<10) - Trigger volumes
```

**Broad-Phase Layers** (4 total):
- `WORLD` - Static geometry
- `MOVING` - Actors, projectiles, dynamic objects
- `DEBRIS` - Corpses and effects
- `SENSOR` - Trigger volumes

**Key Design Decision**: `DYNAMIC_WORLD` does NOT collide with `WATER` layer. Instead, buoyancy forces are applied via `ApplyBuoyancyImpulse()`.

### Core Classes

| Class | File | Purpose |
|-------|------|---------|
| `PhysicsSystem` | physicssystem.cpp | Main manager - initialization, object lifecycle, water |
| `PhysicsTaskScheduler` | mtphysics.cpp | Multi-threaded physics stepping, actor updates |
| `MovementSolver` | movementsolver.cpp | Character movement with collision response |
| `Actor` | actor.cpp | Kinematic character bodies |
| `Object` | object.cpp | Static world objects |
| `DynamicObject` | dynamicobject.cpp | Physics-driven items with buoyancy |
| `RagdollWrapper` | ragdollwrapper.cpp | Runtime ragdoll management |
| `RagdollSettingsBuilder` | ragdollbuilder.cpp | Constructs ragdoll from OSG skeleton |

### Filter Classes (`joltfilters.hpp`)

- `MultiBroadPhaseLayerFilter` - Filter by multiple broad-phase layers
- `MultiObjectLayerFilter` - Filter by multiple object layers
- `MaskedObjectLayerFilter` - Filter by layer bitmask
- `JoltTargetBodiesFilter` - Include/exclude specific bodies

## Dynamic Objects

Dynamic objects use convex approximations of mesh shapes since Jolt's `MeshShape` cannot collide with other meshes or heightfields.

**Shape Types** (configured in `collision-shapes.yaml`):
- `Box` - Default, AABB-based
- `Sphere` - Maximum extent as radius
- `Capsule` - Average XY as radius, Z as height
- `Cylinder` - Same as capsule but cylindrical

**Physics Properties**:
```cpp
linearDamping = 0.1f;   // Prevents infinite sliding
angularDamping = 0.2f;  // Prevents infinite spinning
friction = 0.5f;
restitution = 0.3f;
motionQuality = LinearCast;  // Continuous collision detection
```

## Buoyancy System

Buoyancy is applied per-physics-substep (not per-frame) to maintain frame-rate independence:

```cpp
while (mTimeAccumJolt >= mPhysicsDt)
{
    for (auto& dynObj : mDynamicObjects)
    {
        if (dynObj->isInWaterZone())
            dynObj->updateBuoyancy(waterHeight, gravity, mPhysicsDt);
    }
    mPhysicsSystem->Update(mPhysicsDt, ...);
}
```

**Water Zone Tracking**: Objects have an `mInWaterZone` flag updated once per frame. Only objects within 500 units above water level get buoyancy processing. This optimization prevents O(n) checks every substep for objects far from water.

**Buoyancy Parameters**:
```cpp
buoyancy = 1.2f;      // Slightly buoyant (objects float)
linearDrag = 0.5f;    // Water resistance
angularDrag = 0.05f;  // Rotational damping
```

## Ragdoll System (WIP)

Uses Jolt's built-in `JPH::Ragdoll` with `JPH::RagdollSettings`.

### Physics Hierarchy

20 bones with anatomically correct parent relationships (differs from OSG hierarchy):
```
Pelvis (root)
├── Spine → Spine1 → Spine2
│   ├── Neck → Head
│   ├── L Clavicle → L Upperarm → L Forearm → L Hand
│   └── R Clavicle → R Upperarm → R Forearm → R Hand
├── L Thigh → L Calf → L Foot
└── R Thigh → R Calf → R Foot
```

### Bone Shapes

- **Capsule**: Limbs, spine (most common)
- **Box**: Hands, feet, pelvis
- **Sphere**: Head

### Constraints

Uses `SwingTwistConstraint` with anatomically correct limits:
- Spine: ±9° twist, ~15° swing
- Neck: ±23° twist/swing
- Shoulder: ±34° twist, ~46° swing
- Elbow/Knee: 0-115° (one-way bend)

### Transform Synchronization

`RagdollWrapper::updateBoneTransforms()`:
1. Collect all body world transforms from Jolt
2. Move Bip01 (skeleton root) to follow pelvis physics body
3. Compute local transforms for each bone relative to physics parent
4. Apply via `NifOsg::MatrixTransform::setRotation()`

**Critical**: Uses `OffsetCenterOfMassShape` to keep COM at joint position for proper constraint behavior.

## Thread Safety

- All body access uses `JPH::BodyLockRead/Write`
- `PhysicsTaskScheduler` manages simulation mutex
- Batch body removal via `flushBodyRemovals()` to avoid mid-simulation modifications

## Object Grabbing

Oblivion/Skyrim-style object interaction:

```cpp
grabObject(rayStart, rayDir, maxDistance);   // Start grabbing
updateGrabbedObject(targetPosition);          // Each frame
releaseGrabbedObject(throwVelocity);          // Release with optional throw
```

Also supports ragdoll grabbing for dragging corpses.

## Known Issues / TODOs

| Location | Issue |
|----------|-------|
| `water.cpp:16` | Water uses placeholder 1M x 1M box, needs proper shape |
| `trace.cpp:55` | Motion direction may be backwards |
| `actorconvexcallback.hpp:94` | Standing on actors walking up slopes has issues |
| Ragdoll | WIP - needs thorough testing across skeleton types |

## Performance Considerations

1. **Buoyancy Loop**: Uses water zone tracking to skip objects far from water
2. **Batch Removal**: Bodies queued and removed in batches via `flushBodyRemovals()`
3. **Job System**: Configurable thread count via `Settings::physics().mAsyncNumThreads`
4. **Sleeping**: Dynamic objects sleep when at rest (`AllowSleeping = true`)

## Initialization

Jolt global state (allocator, factory, types) is initialized in `main()` before `PhysicsSystem` construction:

```cpp
JPH::RegisterDefaultAllocator();
JPH::Factory::sInstance = new JPH::Factory();
JPH::RegisterTypes();
```

These persist for application lifetime - do NOT call `UnregisterTypes()` in destructor.

## Debug Rendering

Toggle with `PhysicsSystem::toggleDebugRendering()`. Uses `MWRender::JoltDebugDrawer` for:
- Collision shape visualization
- Contact point display
- Body state (active/sleeping)
