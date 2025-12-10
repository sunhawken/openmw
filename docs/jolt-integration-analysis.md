# Jolt Physics Integration Analysis for OpenMW

## Executive Summary

This document analyzes the migration from Bullet Physics to Jolt Physics in OpenMW. It examines the original Bullet implementation (commit `e7b954a7b6`), compares it with the current Jolt implementation, and evaluates correctness based on Jolt's best practices for open-world games.

---

## Part 1: Bullet Physics Integration (Original)

### Architecture Overview

The original Bullet implementation used the following key components:

#### Core Classes
- **`PhysicsSystem`** - Main physics manager
  - Uses `btCollisionWorld` for collision detection (not full dynamics world)
  - Manages `btBroadphaseInterface`, `btCollisionDispatcher`, `btDefaultCollisionConfiguration`
  - Custom `PhysicsTaskScheduler` for multi-threaded simulation

- **`Actor`** - Character physics body
  - Uses `btConvexShape` for character collision
  - Kinematic body (manually moved, not physics-driven)
  - Custom collision masks for filtering

- **`Object`** - Static world geometry
  - Uses `btCompoundShape` for complex meshes
  - Static collision objects

- **`HeightField`** - Terrain collision
  - Uses `btHeightfieldTerrainShape`

- **`Projectile`** - Spell/arrow collision
  - Uses `btConvexShape` for sweep tests

#### Resource Management
- **`BulletShapeManager`** - Shape caching and instancing
- **`BulletNifLoader`** - NIF file to Bullet shape conversion

### Threading Model

Bullet's threading was implemented via:
1. **`PhysicsTaskScheduler`** - Custom job scheduler
2. **`LockingPolicy`** - Three modes: NoLocks, ExclusiveLocksOnly, AllowSharedLocks
3. **Barriers** - Pre-step, post-step, post-sim barriers for synchronization
4. **Worker threads** - Parallel movement solving

### Key Patterns

1. **Movement Solving**: Custom `MovementSolver` with manual sweep tests
2. **Collision Filtering**: Bitmask-based (`CollisionType_World`, `CollisionType_Actor`, etc.)
3. **Shape Instancing**: Shapes cached and instanced per-object with custom scaling

---

## Part 2: Jolt Physics Best Practices

Based on official Jolt documentation and Horizon Forbidden West GDC talk:

### Body Management

1. **Batch Operations Are Critical**
   - Use `AddBodiesPrepare`/`AddBodiesFinalize` for bulk additions
   - Use `RemoveBodies` for bulk removals
   - Single-body additions result in inefficient broadphase
   - Always call `OptimizeBroadPhase()` after bulk operations

2. **Background Streaming**
   - Bodies can be prepared on background threads without affecting simulation
   - `AddBodiesAbort` allows canceling queued additions (useful when player changes direction)
   - Batch operations are atomic and minimize lock contention

3. **Body ID vs Body Pointer**
   - Always use `BodyID` for references (thread-safe)
   - Use `BodyLockRead`/`BodyLockWrite` for body access
   - Never store raw `Body*` pointers across frames

### Open World Considerations

1. **Large Worlds**
   - Compile with `JPH_DOUBLE_PRECISION` for worlds > few km
   - Use base offset for collision queries near expected results

2. **Cell Streaming**
   - Batch add bodies when loading cells
   - Batch remove bodies when unloading cells
   - Call `OptimizeBroadPhase()` after streaming operations

3. **Wake-up Prevention**
   - Bodies don't automatically wake neighbors when added/removed
   - Manual control prevents cascade wake-ups during streaming

### Threading Best Practices

1. **Lock-free broadphase** - Queries can run parallel to modifications
2. **Job system** - Use Jolt's job system for physics work
3. **Callback safety** - Contact callbacks run multi-threaded during Update()

---

## Part 3: Current Jolt Implementation Analysis

### What Was Done Correctly

#### 1. Batch Body Operations (Excellent)

```cpp
// In mtphysics.cpp
void PhysicsTaskScheduler::beginBatchAdd()
void PhysicsTaskScheduler::endBatchAdd()
{
    JPH::BodyInterface::AddState addState = bodyInterface.AddBodiesPrepare(
        validBodies.data(), static_cast<int>(validBodies.size()));
    bodyInterface.AddBodiesFinalize(
        validBodies.data(), static_cast<int>(validBodies.size()),
        addState, JPH::EActivation::DontActivate);
}
void PhysicsTaskScheduler::optimizeBroadPhase()
```

- Correctly implements batch addition with `AddBodiesPrepare`/`AddBodiesFinalize`
- Calls `OptimizeBroadPhase()` after batch operations
- Used during cell loading in `Scene::insertCell()`

#### 2. Proper BodyID Usage (Excellent)

```cpp
// Example from Actor
JPH::BodyID mPhysicsBody;  // Not Body*

// Example from physicssystem.cpp
JPH::BodyLockRead lock(mPhysicsSystem->GetBodyLockInterface(), ioHit.mBodyID);
if (lock.Succeeded()) {
    const JPH::Body& hitBody = lock.GetBody();
    // Safe access
}
```

- Uses `BodyID` throughout for thread safety
- Properly uses `BodyLockRead`/`BodyLockWrite` for body access
- Validates body existence before access

#### 3. Layer System (Good)

```cpp
namespace Layers {
    static constexpr JPH::ObjectLayer WORLD = 1 << 0;
    static constexpr JPH::ObjectLayer DOOR = 1 << 1;
    static constexpr JPH::ObjectLayer ACTOR = 1 << 2;
    static constexpr JPH::ObjectLayer HEIGHTMAP = 1 << 3;
    static constexpr JPH::ObjectLayer PROJECTILE = 1 << 4;
    static constexpr JPH::ObjectLayer WATER = 1 << 5;
    static constexpr JPH::ObjectLayer DYNAMIC_WORLD = 1 << 8;
    static constexpr JPH::ObjectLayer DEBRIS = 1 << 9;
    // ...
}
```

- Good separation of collision layers
- Proper broadphase layer mapping
- Custom filters for complex collision rules

#### 4. Shape Management (Good)

```cpp
// PhysicsShapeManager - caching and instancing
class PhysicsShapeManager : public ResourceManager
{
    osg::ref_ptr<const PhysicsShape> getShape(VFS::Path::NormalizedView name);
    osg::ref_ptr<PhysicsShapeInstance> getInstance(VFS::Path::NormalizedView name);
};
```

- Proper shape caching and reference counting
- `JPH::Ref<JPH::Shape>` for automatic memory management
- Shape instancing for scaled copies

#### 5. Job System Integration (Good)

```cpp
// Uses Jolt's built-in job systems
if (numThreads > 0) {
    mPhysicsJobSystem = std::make_unique<JPH::JobSystemThreadPool>(
        cMaxPhysicsJobs, cMaxPhysicsBarriers, numThreads);
} else {
    mPhysicsJobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(cMaxPhysicsJobs);
}
```

- Properly uses Jolt's job system
- Thread count configuration from settings
- Single-threaded fallback available

#### 6. Contact Listener System (Good)

```cpp
class JoltContactListener : public JPH::ContactListener
{
    virtual JPH::ValidateResult OnContactValidate(...) override;
    virtual void OnContactAdded(...) override;
};
```

- Proper contact listener for collision events
- Used for projectile hits, dynamic object interactions

#### 7. New Features (Excellent)

- **DynamicObject** - Full dynamic physics for items
- **RagdollWrapper** - Uses Jolt's built-in ragdoll system
- **Grab/Hold System** - Object manipulation
- **Buoyancy** - Water interaction for dynamic objects

### What Could Be Improved

#### 1. Batch Removal Not Fully Utilized

```cpp
// In physicssystem.cpp - Individual removal
void PhysicsSystem::remove(const MWWorld::Ptr& ptr)
{
    mTaskScheduler->syncSimulation();
    // Direct removal...
}

// Batch removal exists but may not be used during cell unload
void PhysicsSystem::flushBodyRemovals()
```

**Issue**: While `flushBodyRemovals()` exists, it's unclear if cell unloading actually uses batch removal. Individual `remove()` calls sync simulation each time, which could be expensive.

**Recommendation**: Ensure cell unloading uses `queueBodyRemoval()` + `flushBodyRemovals()` pattern.

#### 2. Missing AddBodiesAbort Usage

```cpp
// From Jolt docs: "useful when streaming in level sections and
// the player decides to go the other way"
```

**Issue**: No evidence of `AddBodiesAbort()` being used when player direction changes during cell loading.

**Recommendation**: Consider implementing abort mechanism for interrupted cell loads.

#### 3. Double Precision Not Enabled

**Issue**: Large exterior worlds (Tamriel) could benefit from double precision, but `JPH_DOUBLE_PRECISION` doesn't appear to be used.

**Recommendation**: For very large worlds, consider enabling double precision mode. This would require:
- Compiling Jolt with `JPH_DOUBLE_PRECISION`
- Using `JPH::RVec3` consistently (already done in queries)
- Base offset pattern for collision results

#### 4. Movement Solver Still Custom

```cpp
// movementsolver.cpp - Custom implementation
void MovementSolver::move(ActorFrameData& actor, float time,
    const JPH::PhysicsSystem* physicsSystem, const WorldFrameData& worldData)
```

**Status**: This is likely intentional for Morrowind compatibility, but it means:
- Manual trace/sweep operations
- Custom collision response logic
- Not using Jolt's character controller

**Note**: This is probably correct for matching vanilla Morrowind behavior.

#### 5. Projectile Implementation

```cpp
// Projectiles use dynamic bodies but custom collision
class Projectile final : public PtrHolder
{
    void onContactAdded(...) override;
    bool onContactValidate(...) override;
    void setVelocity(osg::Vec3f velocity) override;
};
```

**Issue**: The projectile system removed the original simulation variant and relies on contact callbacks. This works but the Bullet version had a more explicit sweep-based approach.

**Note**: This is a design choice, not necessarily wrong.

#### 6. Scene Integration Coverage

```cpp
// scene.cpp only uses batch for insertion
mPhysics->beginBatchAdd();
insertVisitor.insert(...);
mPhysics->endBatchAdd();
```

**Issue**: Only `insertCell()` uses batch operations. Individual object additions (e.g., spawning items) may not benefit from batching.

**Recommendation**: Consider batching for bulk object spawns (e.g., container contents).

### Potential Issues

#### 1. Thread Safety in Contact Callbacks

```cpp
// In ptrholder.hpp
virtual void onContactAdded(const JPH::Body& withBody,
    const JPH::ContactManifold& inManifold,
    JPH::ContactSettings& ioSettings);
```

Jolt contact callbacks are called from multiple threads during `PhysicsSystem::Update()`. The current implementation needs to ensure:
- No unsafe state modifications
- Proper mutex usage where needed
- No blocking operations

**Current Status**: Implementation appears aware of this (uses mutex in several places).

#### 2. Body UserData Lifetime

```cpp
// Getting UserData
uintptr_t userData = hitBody.GetUserData();
if (userData != 0) {
    PtrHolder* ptrHolder = Misc::Convert::toPointerFromUserData<PtrHolder>(userData);
```

**Issue**: UserData points to PtrHolder objects. If a body is accessed after its PtrHolder is destroyed, this could crash.

**Current Mitigation**:
```cpp
// In remove() and flushBodyRemovals()
// UserData is set to 0 before destruction (via markBodyRemoved)
```

This appears handled correctly.

---

## Part 4: Architectural Comparison

### Similarities

| Aspect | Bullet | Jolt |
|--------|--------|------|
| Actor handling | Kinematic + custom movement | Kinematic + custom movement |
| Static objects | btCompoundShape | JPH::StaticCompoundShape |
| Heightfields | btHeightfieldTerrainShape | JPH::HeightFieldShape |
| Shape caching | BulletShapeManager | PhysicsShapeManager |
| Threading | Custom scheduler | Custom scheduler + Jolt jobs |

### Differences

| Aspect | Bullet | Jolt |
|--------|--------|------|
| World type | btCollisionWorld (no dynamics) | Full PhysicsSystem |
| Dynamic objects | Not supported | DynamicObject class |
| Ragdolls | Not supported | RagdollWrapper |
| Batch operations | N/A | AddBodiesPrepare/Finalize |
| Broadphase | btDbvtBroadphase | Jolt's hierarchical grid |

### New Capabilities

1. **Dynamic Objects**: Items can be pushed, thrown, affected by physics
2. **Ragdolls**: Dead actors use physics-driven ragdoll
3. **Grab System**: Player can pick up and manipulate objects
4. **Buoyancy**: Objects float in water
5. **Better Threading**: Jolt's lock-free broadphase improves streaming

---

## Part 5: Recommendations

### High Priority

1. **Verify batch removal during cell unload**
   - Profile cell transitions
   - Ensure `flushBodyRemovals()` is used for cell unloads

2. **Add performance metrics**
   - Track broadphase efficiency
   - Monitor batch operation sizes

### Medium Priority

3. **Consider AddBodiesAbort for interrupted loads**
   - Would improve streaming responsiveness
   - Low complexity to implement

4. **Document threading guarantees**
   - Contact callback thread safety
   - Body access patterns

### Low Priority

5. **Evaluate double precision**
   - Test world coordinate limits
   - Only needed if precision issues occur far from origin

6. **Profile individual vs batch additions**
   - Spawning items during gameplay
   - Container content instantiation

---

## Part 6: Cell Transition Crash Analysis

This section documents the likely causes of crashes when transitioning between interior and exterior cells.

### Crash Cause #1: No Batch Removal During Cell Unload (CRITICAL)

**Location**: `scene.cpp:360-390`, `physicssystem.cpp:871-907`

```cpp
// scene.cpp - unloadCell() calls remove() for each object individually
for (const auto& ptr : visitor.mObjects)
{
    mPhysics->remove(ptr);  // Each call triggers syncSimulation()!
}

// physicssystem.cpp - remove() syncs EVERY TIME
void PhysicsSystem::remove(const MWWorld::Ptr& ptr)
{
    mTaskScheduler->syncSimulation();  // Called for EVERY object!
    // ...
}
```

**Problem**: When unloading a cell with 100+ objects, this calls `syncSimulation()` 100+ times. Each sync processes simulation results that may reference bodies being destroyed in subsequent iterations. This is both a performance issue and a correctness issue.

**Contrast with cell loading** which correctly uses batch operations:
```cpp
mPhysics->beginBatchAdd();
insertVisitor.insert(...);
mPhysics->endBatchAdd();
```

---

### Crash Cause #2: Buoyancy Accesses Potentially Invalid Cell/Ptr (CRITICAL)

**Location**: `physicssystem.cpp:1221-1248`

```cpp
// In stepSimulation() - runs every frame
for (auto& [_, dynObj] : mDynamicObjects)
{
    MWWorld::Ptr ptr = dynObj->getPtr();
    if (ptr.isEmpty())
        continue;

    const MWWorld::CellStore* cell = ptr.getCell();  // DANGEROUS!
    bool hasWater = cell && cell->getCell()->hasWater();
    float waterHeight = hasWater ? cell->getWaterLevel() : mWaterHeight;
    // ...
    dynObj->updateBuoyancy(waterHeight, gravity, mPhysicsDt);
}
```

**Problems**:
1. `ptr.getCell()` may return a cell that is being unloaded or has been unloaded
2. During cell transition, dynamic objects may have stale cell references
3. The cell pointer could be dangling if the cell was destroyed between frames
4. No synchronization with cell unloading - physics runs while cells are being removed

**This is likely an early implementation issue** - buoyancy was added early and may not have been updated when cell handling changed.

---

### Crash Cause #3: Destructor Order and Non-Batched Body Removal (HIGH)

**Location**: `object.cpp:49-58`, `actor.cpp:149-158`, `heightfield.cpp:54-61`

```cpp
// All these destructors individually remove bodies:
Object::~Object() {
    mPhysicsBody->SetUserData(0);
    mTaskScheduler->removeCollisionObject(mPhysicsBody);  // NOT batch!
    mTaskScheduler->destroyCollisionObject(mPhysicsBody);
}
```

**Problem**: When `mObjects.erase()` is called in `remove()`, it destroys the Object which immediately removes/destroys its body via the destructor. Meanwhile:
- Other physics operations may be referencing that BodyID
- The `markBodyRemoved()` pattern exists but is only used by `flushBodyRemovals()`, not individual removes
- Jolt may still have pending operations referencing this body

---

### Crash Cause #4: Simulation Results with Stale BodyIDs (HIGH)

**Location**: `mtphysics.cpp:424-438`

```cpp
void PhysicsTaskScheduler::syncSimulation()
{
    waitForSimulationBarrier();

    if (mSimulations != nullptr)
    {
        const Visitors::Sync vis{ ... };
        for (auto& sim : *mSimulations)
            std::visit(vis, sim);  // Processes mStandingOn BodyIDs
    }
}
```

The `Sync` visitor in `mtphysics.cpp:234-280` calls `getUserPointer(frameData.mStandingOn)` which tries to access a BodyID. If that body was destroyed between simulation dispatch and sync completion, this crashes.

---

### Crash Cause #5: HeightField Removal Not Synchronized (HIGH)

**Location**: `scene.cpp:395-399`, `heightfield.cpp:54-61`

```cpp
// scene.cpp - happens AFTER object removal loop
if (cell->getCell()->isExterior())
{
    mNavigator.removeHeightfield(...);
    mPhysics->removeHeightField(cellX, cellY);  // Immediate destruction
}

// heightfield.cpp destructor - immediate removal
HeightField::~HeightField() {
    mTaskScheduler->removeCollisionObject(mPhysicsBody);
    mTaskScheduler->destroyCollisionObject(mPhysicsBody);
}
```

**Problem**: Heightfield removal happens AFTER objects are removed but uses the same non-batched approach. If any pending physics queries or simulation results reference the heightfield, crash.

---

### Crash Cause #6: mStandingOnPtr Dangling Pointer Window (MEDIUM)

**Location**: `physicssystem.cpp:884-888`

```cpp
// After syncSimulation(), clear standing references
for (auto& [_, actor] : mActors)
{
    if (actor->getStandingOnPtr() == ptr)
        actor->setStandingOnPtr(MWWorld::Ptr());
}
```

**Problem**: This iterates over `mActors`, but:
- If an actor being removed IS in `mActors`, and another actor is standing on IT, there's a timing window
- This doesn't handle actors standing on heightfields that are being removed
- The check happens after `syncSimulation()` but the standing pointer could have been set during simulation

---

### Crash Cause #7: DynamicObject Map Iteration During Modification (MEDIUM)

**Location**: `physicssystem.cpp:1224-1248`

```cpp
for (auto& [_, dynObj] : mDynamicObjects)
{
    // ... buoyancy code that accesses dynObj
}
```

If `mDynamicObjects` is modified during iteration (e.g., by a contact callback removing an object, or by cell unloading happening concurrently), the iterator is invalidated.

---

### Summary Table

| Issue | Severity | Symptom |
|-------|----------|---------|
| No batch removal during unload | **Critical** | Crash during cell exit |
| Buoyancy accesses stale cell | **Critical** | Crash when dynamic objects exist during transition |
| Non-batched destructor removal | **High** | Random crashes during unload |
| Stale BodyIDs in simulation results | **High** | Crash in syncSimulation() |
| HeightField immediate removal | **High** | Crash exiting exterior cells |
| mStandingOnPtr timing window | **Medium** | Occasional crash when standing on objects |
| DynamicObject iteration | **Medium** | Crash if objects removed during physics step |

---

### Recommended Fix Priority

1. **Add batch removal to cell unloading** (mirrors cell loading pattern)
2. **Guard buoyancy loop** against invalid cells/ptrs, or defer to after cell transition completes
3. **Single syncSimulation() before any removals** instead of per-object
4. **Use queueBodyRemoval() pattern** for all removals during cell unload
5. **Synchronize heightfield removal** with body batch removal

---

## Part 7: Conclusion

The Jolt integration in OpenMW is **well-implemented** and follows most Jolt best practices:

**Strengths:**
- Proper batch body operations for cell streaming
- Correct BodyID usage and thread-safe access patterns
- Good layer system for collision filtering
- Excellent new features (dynamics, ragdolls, grab system)
- Proper resource management with shape caching

**Areas for Improvement:**
- Verify batch removal is used everywhere it should be
- Consider AddBodiesAbort for better streaming cancellation
- Ensure contact callbacks remain thread-safe as features grow

**Overall Assessment:** The integration is production-quality and represents a significant improvement over the Bullet implementation, particularly for dynamic gameplay features. The core architecture correctly follows Jolt's design patterns for open-world games.

---

## References

- [Jolt Physics GitHub](https://github.com/jrouwe/JoltPhysics)
- [Jolt Architecture Documentation](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md)
- [GDC 2022: Architecting Jolt Physics for Horizon Forbidden West](https://www.guerrilla-games.com/read/architecting-jolt-physics-for-horizon-forbidden-west)
- [Jolt API Documentation](https://jrouwe.github.io/JoltPhysics/)
