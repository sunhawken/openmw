# Ragdoll Integration Analysis for OpenMW

## Executive Summary

This document provides a comprehensive analysis of how to properly integrate Jolt Physics ragdoll into OpenMW's animation system. It covers the ideal approach, audits the three relevant systems (OpenMW animation, Jolt ragdoll API, current implementation), and provides a reference for debugging.

---

## Part 1: The Ideal Ragdoll Integration Approach

### 1.1 Understanding the Core Challenge

The fundamental challenge of ragdoll integration is **bridging two different transform systems**:

1. **Animation System** (OSG/NifOsg): Uses hierarchical transforms where each bone stores a local transform relative to its parent. The world transform is computed by multiplying up the chain.

2. **Physics System** (Jolt): Uses independent rigid bodies with world-space transforms. Constraints enforce relationships between bodies.

The key insight is that **these systems use transforms differently**:
- Animation: `worldTransform = localTransform × parentWorldTransform` (hierarchical)
- Physics: Each body has its own independent `worldTransform` (flat)

### 1.2 The Ideal Approach

The ideal ragdoll integration follows these principles:

#### Principle 1: Use Native Bone Rotations

**DO NOT calculate synthetic body rotations** (e.g., "rotate Z to point along bone direction").

Instead, **use the bone's native world rotation directly from OSG**:

```cpp
// CORRECT: Use native bone rotation
osg::Matrix boneWorldMatrix = getWorldMatrix(boneNode);
osg::Quat bodyRotation = boneWorldMatrix.getRotate();  // Native!

// WRONG: Calculate synthetic rotation
osg::Quat bodyRotation = makeRotationAlongZ(boneDirection);  // Creates mismatch!
```

**Why?** OpenMW's animation system works with native bone rotations from NIF files. Each bone has an arbitrary local coordinate frame - there is NO consistent convention like "Z points toward child". Calculating synthetic rotations creates a mismatch between physics and rendering.

#### Principle 2: Body Position = Joint Position

Following Jolt's design philosophy:
> "The assumption is that the body position/orientation IS the joint position/orientation."

Each physics body should be positioned at the **bone's origin** (the joint connecting it to its parent), NOT at the center of the collision shape.

```cpp
// Body at joint position
part.mPosition = Misc::Convert::toJolt<JPH::RVec3>(boneWorldPosition);

// Shape offset from joint to shape center (e.g., halfway along bone)
JPH::Vec3 shapeOffset(0, 0, boneLength * 0.5f);  // In bone-local space
shape = new JPH::RotatedTranslatedShape(shapeOffset, rotation, baseShape);
```

#### Principle 3: Center of Mass at Joint

For constraints to work correctly, the center of mass must be at the body origin (joint position):

```cpp
// After creating the offset shape, shift COM back to origin
JPH::Vec3 comOffset = shape->GetCenterOfMass();
if (comOffset.LengthSq() > 0.0001f) {
    shape = new JPH::OffsetCenterOfMassShape(shape, -comOffset);
}
```

This is critical because `EConstraintSpace::LocalToBodyCOM` expects the COM at the body origin.

#### Principle 4: Constraint Axes in Body-Local Space

Use `LocalToBodyCOM` constraint space and express all axes relative to each body's local coordinate frame:

```cpp
settings->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

// Twist axis = bone direction, expressed in each body's local frame
osg::Vec3f worldTwistAxis = (childPos - parentPos).normalize();
settings->mTwistAxis1 = parentRot.inverse() * worldTwistAxis;  // Parent local
settings->mTwistAxis2 = childRot.inverse() * worldTwistAxis;   // Child local

// Child's constraint position is at its origin (the joint)
settings->mPosition2 = JPH::Vec3::sZero();

// Parent's constraint position is offset to the joint location
osg::Vec3f parentToJoint = jointWorldPos - parentWorldPos;
settings->mPosition1 = parentRot.inverse() * parentToJoint;  // Parent local
```

#### Principle 5: Transform Sync Using NifOsg Methods

When syncing physics back to rendering, use `NifOsg::MatrixTransform`'s specialized methods that preserve scale:

```cpp
auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(boneNode);
if (nifTransform) {
    nifTransform->setRotation(localRot);     // Preserves mScale
    nifTransform->setTranslation(localPos);  // Preserves mRotationScale
} else {
    // Fallback for non-NIF bones
    osg::Matrix localMatrix;
    localMatrix.makeRotate(localRot);
    localMatrix.setTrans(localPos);
    boneNode->setMatrix(localMatrix);
}
```

**DO NOT** call `skeleton->markDirty()` during ragdoll updates - this clears the bone cache and is only needed when the skeleton structure changes.

### 1.3 The Ideal Data Flow

```
INITIALIZATION:

  OSG Skeleton                  Jolt Ragdoll
  ============                  ============

  For each bone in processing order:
    1. Get native world matrix from OSG
    2. Body position = bone world position
    3. Body rotation = bone native rotation (NOT calculated!)
    4. Shape offset = direction to child in bone-local space
    5. Constraint anchor: child at (0,0,0), parent at (childPos - parentPos) in parent-local
    6. Constraint axes: bone direction in each body's local space

RUNTIME (each frame):

  Physics Step
       |
       v
  For each ragdoll bone:
    1. Get body world transform from Jolt
    2. Subtract shape offset (in body's local frame) to get joint position
    3. Determine parent's world transform:
       - If parent is physics bone: use its physics transform
       - Otherwise: use OSG computed transform
    4. Compute local transform: local = inverse(parentWorld) * thisWorld
    5. Apply via NifOsg::setRotation() and setTranslation()
       |
       v
  Rendering
```

### 1.4 Key Invariants

These must always be true for a correct implementation:

1. **Body origin = Bone origin**: `body.GetPosition() - body.GetRotation() * shapeOffset == bone.getWorldPosition()`

2. **Constraint at joint**: Both constraint anchors, when transformed to world space, should be at the same point (the joint)

3. **Axes align**: Twist axis points along the bone direction in world space when transformed from either body's local space

4. **Scale preserved**: `NifOsg::MatrixTransform.mScale` unchanged after ragdoll updates

5. **Hierarchy respected**: Parent bones always update before child bones

---

## Part 2: OpenMW Animation System Audit

### 2.1 How Skeletons Are Loaded

**File:** `components/nifosg/nifloader.cpp`

1. NIF files contain `NiNode` hierarchies with `mIsBone` flags
2. Each node has an `NiTransform` with:
   - `mRotation`: 3x3 rotation/scale matrix (arbitrary orientation)
   - `mTranslation`: position relative to parent
   - `mScale`: uniform scale factor

3. NifLoader creates `NifOsg::MatrixTransform` nodes that preserve the NIF decomposition:
   ```cpp
   class NifOsg::MatrixTransform {
       float mScale;                  // Preserved separately
       Nif::Matrix3 mRotationScale;   // Preserved separately
       // The 4x4 matrix is computed from these components
   };
   ```

### 2.2 How Animations Are Applied

**Files:** `components/nifosg/controller.cpp`, `apps/openmw/mwrender/animation.cpp`

1. `KeyframeController` interpolates keyframes (quaternion SLERP for rotation)
2. Applies via component setters that preserve the decomposition:
   ```cpp
   node->setRotation(interpolatedQuat);    // Updates mRotationScale
   node->setTranslation(interpolatedPos);  // Updates translation column
   node->setScale(interpolatedScale);      // Updates mScale
   ```

3. Animation priorities and blend masks allow overlaying animations (e.g., walking while attacking)

### 2.3 How Bone Transforms Update Each Frame

**File:** `components/sceneutil/skeleton.cpp`

1. `Skeleton::updateBoneMatrices()` called during traversal
2. `Bone::update()` computes skeleton-space matrix:
   ```cpp
   void Bone::update(const osg::Matrixf* parentMatrix) {
       if (parentMatrix)
           mMatrixInSkeletonSpace = mNode->getMatrix() * (*parentMatrix);
       else
           mMatrixInSkeletonSpace = mNode->getMatrix();

       for (auto& child : mChildren)
           child->update(&mMatrixInSkeletonSpace);
   }
   ```

3. RigGeometry uses these matrices for mesh skinning

### 2.4 Morrowind Skeleton Structure

**Standard Bip01 Hierarchy:**
```
Bip01 Pelvis (root)
├── Bip01 Spine → Spine1 → Spine2
│   ├── Bip01 Neck → Head
│   ├── Bip01 L Clavicle → L Upperarm → L Forearm → L Hand
│   └── Bip01 R Clavicle → R Upperarm → R Forearm → R Hand
├── Bip01 L Thigh → L Calf → L Foot
└── Bip01 R Thigh → R Calf → R Foot
```

**Important:** Arms attach to Spine2 (not Neck), Legs attach to Pelvis (not Spine).

### 2.5 Key Characteristics

1. **Bone orientations are arbitrary**: No consistent "Z points forward" convention
2. **Scale stored separately**: Must use `setRotation()`/`setTranslation()`, not `setMatrix()`
3. **Hierarchical computation**: Parent must update before child
4. **No intermediate calculation**: Animations use native NIF rotations directly

---

## Part 3: Jolt Ragdoll API Audit

### 3.1 RagdollSettings Structure

```cpp
class RagdollSettings {
    Ref<Skeleton> mSkeleton;           // Joint hierarchy definition
    PartVector mParts;                 // One Part per joint
    AdditionalConstraintVector mAdditionalConstraints;  // Non-hierarchical

    Ragdoll* CreateRagdoll(GroupID, userData, PhysicsSystem*);
    bool Stabilize();                  // Adjust mass ratios for stability
    void DisableParentChildCollisions(); // Prevent self-collision
};

class Part : public BodyCreationSettings {
    Ref<TwoBodyConstraintSettings> mToParent;  // nullptr for root
};
```

### 3.2 Ragdoll Runtime Class

```cpp
class Ragdoll {
    // Lifecycle
    void AddToPhysicsSystem(EActivation);
    void RemoveFromPhysicsSystem();
    void Activate();

    // Pose control
    void SetPose(const SkeletonPose&);           // Instant teleport
    void GetPose(SkeletonPose&);                 // Read current state
    void DriveToPoseUsingKinematics(...);        // Velocity-based animation blend
    void DriveToPoseUsingMotors(...);            // Motor-driven animation blend

    // Query
    size_t GetBodyCount();
    BodyID GetBodyID(int index);
    size_t GetConstraintCount();
    TwoBodyConstraint* GetConstraint(int index);
    bool IsActive();
};
```

### 3.3 SkeletonPose

Jolt's `SkeletonPose` maintains two parallel representations:
- **JointStateVector**: Local rotations/translations relative to parents
- **Mat44Vector**: World-space transformation matrices

These can be converted between each other via `CalculateJointMatrices()` and `CalculateJointStates()`.

### 3.4 Constraint Space Modes

**LocalToBodyCOM** (recommended for ragdoll):
- Positions and axes expressed in each body's local frame relative to center of mass
- COM should be at body origin (use `OffsetCenterOfMassShape`)

**WorldSpace** (alternative):
- Positions and axes in world coordinates
- Simpler but less robust to body movement

### 3.5 Key Jolt Design Principles

1. **Body origin = Joint**: The body's position/rotation represents the joint, shape is offset
2. **COM at origin**: Use `OffsetCenterOfMassShape` to shift COM to body origin
3. **Stability tools**: `Stabilize()` adjusts mass ratios, `DisableParentChildCollisions()` prevents self-collision
4. **Multiple drive modes**: Kinematic, motor-driven, or pure dynamic simulation

---

## Part 4: Current Implementation Audit

### 4.1 File Structure

| File | Purpose |
|------|---------|
| `ragdollwrapper.hpp/cpp` | Main wrapper using Jolt's `JPH::Ragdoll` |
| `ragdollbuilder.hpp/cpp` | Builds `RagdollSettings` from OSG skeleton |
| `ragdoll.hpp/cpp` | Legacy implementation (unused) |

### 4.2 RagdollSettingsBuilder

**Location:** [apps/openmw/mwphysics/ragdollbuilder.cpp](apps/openmw/mwphysics/ragdollbuilder.cpp)

**Responsibilities:**
- Traverse OSG skeleton and identify physics bones
- Create collision shapes for each bone (capsule, box, sphere)
- Estimate bone sizes from parent-child distances
- Configure constraints with joint limits
- Normalize masses to total body weight
- Return bone mappings connecting Jolt indices to OSG nodes

**Key Data Structures:**

```cpp
struct BoneMapping {
    int joltJointIndex;              // Index in Jolt skeleton
    osg::MatrixTransform* osgNode;   // OSG bone node
    osg::Vec3f shapeOffset;          // Offset from joint to shape center
    std::string boneName;
    std::string physicsParentName;   // May differ from OSG parent
    osg::Vec3f boneDirection;        // Direction bone points (normalized)
    osg::Quat bodyRotation;          // Physics body rotation
    osg::Quat originalBoneWorldRot;  // Original OSG bone rotation
};
```

**Physics Hierarchy:**
The builder uses an explicit physics hierarchy (`sPhysicsHierarchy` map) that defines correct anatomical parent-child relationships, which may differ from the OSG scene graph structure.

### 4.3 RagdollWrapper

**Location:** [apps/openmw/mwphysics/ragdollwrapper.cpp](apps/openmw/mwphysics/ragdollwrapper.cpp)

**Responsibilities:**
- Create and own `JPH::Ragdoll` instance
- Sync physics transforms back to OSG bones each frame
- Handle impulse application
- Provide query interface (position, body IDs, at-rest status)

**Transform Sync Process (`updateBoneTransforms`):**

1. **Pass 1**: Collect all body world transforms from Jolt
2. **Pass 2**: For each bone:
   - Find OSG parent's world transform (from physics if also ragdoll bone, else from OSG)
   - Compute local transform relative to parent
   - Apply using `NifOsg::MatrixTransform::setRotation()`/`setTranslation()`

### 4.4 Shape Creation

**Shape types by bone:**
- **Capsule**: Limbs, spine (most common)
- **Box**: Hands, feet, pelvis
- **Sphere**: Head

**Shape offset calculation:**
- Shapes are offset from joint origin toward child bone
- Uses `RotatedTranslatedShape` for positioning
- Uses `OffsetCenterOfMassShape` to move COM to joint

**Size estimation:**
- Length: Distance from bone to first child bone
- Width: Proportional to length (30%)
- Scale factor: 0.7 to reduce overlap

### 4.5 Constraint Configuration

**Constraint type:** `SwingTwistConstraint`

**Space:** `LocalToBodyCOM`

**Position anchors:**
- Child body: `(0, 0, 0)` - constraint at body origin (joint)
- Parent body: `parentRot.inverse() * (jointPos - parentPos)` - offset to joint in parent-local space

**Axes:**
- Twist axis: Bone direction transformed to each body's local space
- Plane axis: Perpendicular to twist, computed via cross product with world up

**Joint limits (examples):**

| Bone Type | Swing Limit | Twist Range |
|-----------|-------------|-------------|
| Spine | ~15° | ±9° |
| Neck | ~23° | ±23° |
| Shoulder | ~46° | ±34° |
| Elbow | ~9° | 0° to 115° |
| Hip | ~40° | ±14° |
| Knee | ~9° | 0° to 115° |

### 4.6 Physics System Integration

**Activation:** `PhysicsSystem::activateRagdoll()` called when actor dies
- Disables actor's kinematic collision body
- Creates `RagdollWrapper`
- Enforces maximum ragdoll limit (removes oldest at-rest)

**Updates:** `PhysicsSystem::updateRagdolls()` called each frame after physics step
- Calls `updateBoneTransforms()` on all active ragdolls

**Storage:** `RagdollMap` keyed by actor's cell ref base

---

## Part 5: Implementation Comparison

| Aspect | Ideal Approach | Current Implementation |
|--------|----------------|------------------------|
| Body rotation source | Native bone rotation | Native bone rotation (`worldMatrix.getRotate()`) |
| Body position | At joint origin | At joint origin |
| Shape offset | In native bone-local space | In calculated local space (`localBoneDir`) |
| COM location | At body origin | At body origin (`OffsetCenterOfMassShape`) |
| Constraint space | `LocalToBodyCOM` | `LocalToBodyCOM` |
| Constraint positions | Child at origin, parent offset | Child at origin, parent offset |
| Constraint axes | In native local frames | Transformed to local frames |
| Transform sync method | `NifOsg::setRotation()`/`setTranslation()` | `NifOsg::setRotation()`/`setTranslation()` |
| Parent resolution | Physics transform if ragdoll, else OSG | Physics transform if ragdoll, else OSG |
| Processing order | Root to leaf | Fixed order (processingOrder array) |
| Jolt ragdoll class | Use built-in `JPH::Ragdoll` | Uses built-in `JPH::Ragdoll` |
| Stability features | `Stabilize()`, collision filtering | `Stabilize()`, `DisableParentChildCollisions()` |

---

## Part 6: Debugging Reference

### 6.1 Key Values to Verify

**Body placement:**
```cpp
// For each bone, verify:
osg::Vec3f jointFromBody = bodyWorldPos - bodyWorldRot * shapeOffset;
osg::Vec3f jointFromOSG = getWorldMatrix(boneNode).getTrans();
// These should be equal (or very close)
```

**Constraint anchors:**
```cpp
// Both anchors should transform to the same world point:
osg::Vec3f anchor1World = parentPos + parentRot * mPosition1;
osg::Vec3f anchor2World = childPos + childRot * mPosition2;
// These should be equal
```

**Constraint axes:**
```cpp
// Twist axes should align in world space:
osg::Vec3f twist1World = parentRot * mTwistAxis1;
osg::Vec3f twist2World = childRot * mTwistAxis2;
// These should be parallel (dot product ~1.0)

// Plane axis perpendicular to twist:
float dot = twistAxis.dot(planeAxis);
// Should be ~0
```

### 6.2 Visualization Suggestions

1. **Draw constraint anchors**: Spheres at both anchor positions in world space
2. **Draw constraint axes**: Lines showing twist (red) and plane (green) axes
3. **Draw body frames**: RGB axes at each body's origin showing its orientation
4. **Draw shape bounds**: Wireframe of actual collision shapes

### 6.3 Incremental Testing

1. **Single bone**: Just pelvis - does it fall and collide correctly?
2. **Two bones**: Pelvis + spine - does the constraint hold them together?
3. **Locked joints**: Set all limits to 0 - is ragdoll completely rigid?
4. **Full ragdoll**: All bones with normal limits

### 6.4 Logging Points

Useful values to log during initialization:
- Bone world position and rotation (from OSG)
- Body position and rotation (sent to Jolt)
- Shape offset vector
- Constraint anchor positions (both bodies)
- Constraint axes (both bodies)
- Joint limits

Useful values to log during runtime:
- Body positions after physics step
- Computed local transforms before applying to OSG
- Parent world transforms used for local computation

---

## Appendix A: Morrowind Bone Names

Standard Bip01 skeleton bones used in physics:

```
bip01 pelvis        (root)
bip01 spine
bip01 spine1
bip01 spine2
bip01 neck
bip01 head
bip01 l clavicle
bip01 l upperarm
bip01 l forearm
bip01 l hand
bip01 r clavicle
bip01 r upperarm
bip01 r forearm
bip01 r hand
bip01 l thigh
bip01 l calf
bip01 l foot
bip01 r thigh
bip01 r calf
bip01 r foot
```

---

## Appendix B: Key Source Files

| File | Purpose |
|------|---------|
| `apps/openmw/mwphysics/ragdollwrapper.cpp` | Runtime ragdoll management |
| `apps/openmw/mwphysics/ragdollbuilder.cpp` | Ragdoll construction from skeleton |
| `apps/openmw/mwphysics/physicssystem.cpp` | Ragdoll lifecycle integration |
| `components/nifosg/matrixtransform.cpp` | NIF-aware transform node |
| `components/sceneutil/skeleton.cpp` | Bone hierarchy and matrix updates |
| `components/nifosg/controller.cpp` | Animation keyframe application |
| `Jolt/Physics/Ragdoll/Ragdoll.h` | Jolt ragdoll API |

---

## Appendix C: References

- Jolt RagdollSettings: https://jrouwe.github.io/JoltPhysicsDocs/
- Jolt SwingTwistConstraint documentation
- Jolt GitHub discussions on ragdoll setup: https://github.com/jrouwe/JoltPhysics/discussions/933
