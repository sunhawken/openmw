# Ragdoll Debug Notes

## Problem Description

Actors who die create "piles of connected body parts" where:
- Body parts can rotate freely
- Parts are only loosely connected with gaps between them
- Bones don't maintain original skeleton structure
- Ragdoll "explodes" or drifts apart on activation

## Current Status (December 2024)

**Active implementation:** `RagdollWrapper` + `RagdollSettingsBuilder` (uses Jolt's `JPH::Ragdoll`)
**Legacy implementation:** `ragdoll.cpp` (unused, should be removed)

---

## CRITICAL INSIGHT: How OpenMW Animation System Works

### The Fundamental Difference from Standard Ragdoll Assumptions

OpenMW's animation system does **NOT** use calculated body rotations. Instead, it works with the **native bone transforms** directly:

1. **Morrowind NIFs store transforms as:**
   - `Nif::Matrix3 mRotation` - 3x3 rotation/scale matrix (arbitrary orientation)
   - `osg::Vec3f mTranslation` - position relative to parent
   - `float mScale` - uniform scale

2. **OpenMW preserves this decomposition** via `NifOsg::MatrixTransform`:
   ```cpp
   float mScale;              // Separate scale value
   Nif::Matrix3 mRotationScale;  // Separate 3x3 rotation matrix
   ```

3. **Animations modify components individually:**
   ```cpp
   node->setRotation(rotation);    // From keyframe quaternion
   node->setTranslation(translation);
   node->setScale(scale);
   ```

4. **Skeleton-space matrices computed bottom-up:**
   ```cpp
   mMatrixInSkeletonSpace = mNode->getMatrix() * (*parentMatrixInSkeletonSpace);
   ```

### Why Our Ragdoll Approach Is Wrong

**Current approach (BROKEN):**
- Calculate a synthetic `bodyRotation` that aligns Z with bone direction
- Use this rotation for physics bodies
- Try to convert back when syncing transforms

**Problem:** This creates a mismatch between:
- Physics body orientation (calculated, Z-aligned)
- OSG bone orientation (native, arbitrary)

**The conversion in `updateBoneTransforms()` is mathematically correct but practically fails** because small numerical errors compound through the skeleton hierarchy.

---

## How OpenMW Animation Actually Works

### Key Components

1. **`NifOsg::MatrixTransform`** (`components/nifosg/matrixtransform.cpp`)
   - Stores rotation and scale SEPARATELY from the 4x4 matrix
   - Allows animations to modify individual components without losing information
   - Handles column/row major conversion between NIF and OSG

2. **`NifOsg::KeyframeController`** (`components/nifosg/controller.cpp`)
   - Interpolates animation keyframes (quaternion SLERP for rotations)
   - Applies transforms via `setRotation()`, `setTranslation()`, `setScale()`
   - Does NOT calculate any "corrected" rotations

3. **`SceneUtil::Skeleton`** (`components/sceneutil/skeleton.cpp`)
   - Manages bone hierarchy
   - Computes skeleton-space matrices by multiplying local * parent
   - Used for skinning (RigGeometry)

### The Critical Insight

**OpenMW animations work because they use the NATIVE bone rotations.**

The rotation stored in the NIF file for each bone defines that bone's local coordinate frame. Animations provide rotation deltas (quaternions) that are applied relative to this native frame. The skinning system expects these native rotations.

---

## Failed Attempts and What We Learned

### Attempt 1: WorldSpace Constraint Mode
- Used `JPH::EConstraintSpace::WorldSpace`
- Set both position anchors to child bone's world position
- Set twist/plane axes to world-space bone direction
- **Result:** Still disjointed. Constraint positions correct but axes may be wrong.

### Attempt 2: LocalToBodyCOM with Calculated Rotations
- Switched to `JPH::EConstraintSpace::LocalToBodyCOM`
- Calculated parent's local anchor position: `parentRot.inverse() * (jointPos - parentPos)`
- Transformed twist/plane axes to each body's local space
- **Result:** Still disjointed. The calculated body rotations don't match native bone orientations.

### Root Issue Identified

The problem is that we're using **calculated physics body rotations** (from `makeRotationAlongZ`) that differ from the **native bone rotations**. This means:

1. Constraint axes are calculated relative to wrong coordinate frames
2. The shape offsets don't align with actual bone geometry as expected
3. Transform sync introduces cumulative errors

---

## Proposed Solution: Use Native Bone Rotations

Instead of calculating synthetic body rotations, we should:

### Option A: Use Native Bone Rotations for Bodies

```cpp
// Instead of:
osg::Quat bodyRotation = makeRotationAlongZ(boneDirection);

// Use:
osg::Quat bodyRotation = boneWorldMatrix.getRotate();  // Native rotation
```

Then for shapes:
- Don't assume shape offset is along "local Z"
- Calculate the actual offset vector in the bone's native local space
- Use the bone's native axes for constraint configuration

### Option B: Simplified Approach - Use Bone Positions Only

Since Jolt says "body position IS joint position":
1. Position each body at the bone's world position (correct)
2. Use the bone's **native world rotation** (not calculated)
3. For shapes, offset in the direction of the CHILD bone (in bone-local space)
4. For constraints, calculate axes based on relative bone positions

### Option C: Match How Animations Work

The animation system uses:
1. Native bone local rotations (from NIF)
2. Animation deltas applied as quaternion multiplication
3. Hierarchy-based accumulation (local * parent = skeleton-space)

For ragdoll, we should:
1. Store native bone world rotation at ragdoll creation
2. After physics step, compute delta from initial physics rotation
3. Apply that delta to the stored native rotation
4. This is what `updateBoneTransforms` tries to do, but it fails due to accumulated errors

---

## Morrowind Skeleton Specifics

### Bone Orientations Are Arbitrary

From debug logging:
```
bip01 spine:     localZ_dot_boneDir = -0.17  (perpendicular!)
bip01 l thigh:   localZ_dot_boneDir = -1.16  (opposite!)
bip01 l clavicle: localZ_dot_boneDir = 0.98  (aligned)
```

Each bone has its own arbitrary local coordinate frame. There is NO consistent convention like "Z points toward child".

### Transform Storage

```cpp
// NIF stores:
struct NiTransform {
    Matrix3 mRotation;     // 3x3 rotation (can include scale)
    osg::Vec3f mTranslation;
    float mScale;
};

// Converted to OSG:
osg::Matrixf toMatrix() const {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            transform(j, i) = mRotation.mValues[i][j] * mScale;  // Column/row swap
}
```

### Hierarchy

```
Pelvis (root)
├── Spine → Spine1 → Spine2
│   ├── Neck → Head
│   ├── L Clavicle → L Upperarm → L Forearm → L Hand
│   └── R Clavicle → R Upperarm → R Forearm → R Hand
├── L Thigh → L Calf → L Foot
└── R Thigh → R Calf → R Foot
```

Arms attach to Spine2 (NOT Neck), Legs attach to Pelvis (NOT Spine).

---

## Next Steps to Try

### 1. Use Native Rotations for Bodies
```cpp
// In RagdollSettingsBuilder::build():
osg::Matrix worldMatrix = getWorldMatrix(boneNode);
osg::Quat bodyRotation = worldMatrix.getRotate();  // Use native, not calculated
part.mRotation = Misc::Convert::toJolt(bodyRotation);
```

### 2. Fix Shape Offsets for Native Frames
The shape offset should be along the direction to the child bone, expressed in the bone's native local frame:
```cpp
osg::Vec3f boneDir = childWorldPos - worldPos;  // World space direction
osg::Vec3f localDir = bodyRotation.inverse() * boneDir;  // To bone local space
// Offset shape along localDir, not along (0,0,1)
```

### 3. Fix Constraint Axes for Native Frames
```cpp
// Twist axis should be bone direction in each body's native local frame
osg::Vec3f parentLocalTwist = parentNativeRot.inverse() * boneDir;
osg::Vec3f childLocalTwist = childNativeRot.inverse() * boneDir;
```

### 4. Simplify Transform Sync
If we use native rotations, sync becomes simpler:
```cpp
// Physics gives us: currentRot (world quaternion)
// This IS the bone's world rotation, directly usable
osg::Quat osgWorldRot = currentPhysicsRot;  // No conversion needed!
```

---

## Jolt Ragdoll Key Principle

From Jolt documentation:
> "The assumption is that the body position/orientation IS the joint position/orientation. E.g. a capsule shape would be offset using a RotatedTranslatedShape so that it is correctly placed if you put the body on the joint."

This means:
1. **Body origin = Joint position** (where bone connects to parent)
2. **Body rotation = Joint orientation** (defines local coordinate frame for shape offset)
3. **Shape is offset** from body origin using `RotatedTranslatedShape`
4. **COM must be at body origin** using `OffsetCenterOfMassShape`

**KEY:** Jolt doesn't care WHAT orientation you use, as long as:
- Shape offset is expressed in that orientation's local frame
- Constraint axes are expressed in that orientation's local frame
- All bodies use consistent conventions

---

## Files Modified

- `ragdollbuilder.cpp`: Added `makeRotationAlongZ()`, calculate body rotation from bone direction
- `ragdollbuilder.hpp`: Added `boneDirection`, `bodyRotation` to `BoneMapping`
- `createConstraintSettings()`: Tried both `WorldSpace` and `LocalToBodyCOM` modes

## Files to Investigate

- `ragdollwrapper.cpp`: `updateBoneTransforms()` - simplify if using native rotations
- `ragdollbuilder.cpp`: Switch to native bone rotations, fix shape offsets

---

## Testing Checklist

- [ ] Single joint test (pelvis + spine only)
- [ ] Verify body positions match joint positions (debug vis)
- [ ] Verify constraint anchor points align (debug vis)
- [ ] Check transform sync produces correct visual result
- [ ] Full skeleton with all bones
- [ ] Ragdoll doesn't explode on activation
- [ ] Parts stay connected during simulation
- [ ] Grabbing ragdoll with mouse moves connected parts together

## References

- Jolt RagdollSettings: https://jrouwe.github.io/JoltPhysicsDocs/5.0.0/class_ragdoll_settings.html
- Jolt SwingTwistConstraint: https://jrouwe.github.io/JoltPhysics/_swing_twist_constraint_8h_source.html
- Jolt Constraint Space: https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Constraints/Constraint.h
- Jolt Discussion on Ragdoll Setup: https://github.com/jrouwe/JoltPhysics/discussions/933
- OpenMW Animation: `components/nifosg/controller.cpp`, `components/nifosg/matrixtransform.cpp`
- OpenMW Skeleton: `components/sceneutil/skeleton.cpp`
