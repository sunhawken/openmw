# Ragdoll System Refactor Plan

## Problem Statement

The current ragdoll implementation has a critical bug where constraint axes are incorrectly configured, causing bones to fly apart. The implementation also hardcodes Morrowind's Bip01 skeleton, which won't work for Oblivion, Skyrim, or other future game support.

## Solution: Use Jolt's Built-in Ragdoll System

Instead of manually creating bodies and constraints, we should use Jolt's `JPH::Ragdoll` and `JPH::RagdollSettings` classes which:
- Handle constraint space conversion correctly
- Provide proper collision filtering between parent/child parts
- Support pose-driven animation (for future active ragdoll features)
- Are well-tested and maintained

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        PhysicsSystem                            │
│  - Manages RagdollMap (MWWorld::Ptr -> RagdollWrapper)         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                      RagdollWrapper                             │
│  - Owns JPH::Ragdoll instance                                  │
│  - Owns JPH::RagdollSettings (for pose reset)                  │
│  - Maps JPH joints to OSG bone nodes                           │
│  - Handles bone transform sync                                  │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    RagdollSettingsBuilder                       │
│  - Builds JPH::Skeleton from SceneUtil::Skeleton               │
│  - Creates JPH::RagdollSettings::Part for each bone            │
│  - Configures constraints with sensible defaults               │
│  - Rig-agnostic: works with any skeleton hierarchy             │
└─────────────────────────────────────────────────────────────────┘
```

## Key Components

### 1. RagdollSettingsBuilder (New Class)

Dynamically creates `JPH::RagdollSettings` from any OSG skeleton:

```cpp
class RagdollSettingsBuilder
{
public:
    struct JointConfig
    {
        float swingLimit = 0.5f;      // Default swing cone angle (radians)
        float twistLimit = 0.3f;      // Default twist range (radians)
        float mass = 1.0f;            // Relative mass
        ShapeType shapeType = Capsule;
    };

    // Build settings from OSG skeleton
    static JPH::Ref<JPH::RagdollSettings> build(
        SceneUtil::Skeleton* osgSkeleton,
        float actorScale,
        const JointConfigMap* overrides = nullptr  // Optional per-bone config
    );

private:
    // Recursively traverse OSG skeleton to build Jolt skeleton
    static void buildJoltSkeleton(
        JPH::Skeleton* joltSkeleton,
        osg::MatrixTransform* node,
        int parentIndex,
        std::vector<BoneMapping>& mappings
    );

    // Create collision shape for a bone
    static JPH::Ref<JPH::Shape> createBoneShape(
        osg::MatrixTransform* bone,
        osg::MatrixTransform* childBone,  // May be null for leaf bones
        const JointConfig& config,
        float scale
    );

    // Create constraint settings between parent and child
    static JPH::Ref<JPH::TwoBodyConstraintSettings> createConstraint(
        const osg::Matrix& parentWorld,
        const osg::Matrix& childWorld,
        const JointConfig& config
    );
};
```

Key design decisions:
- **Rig-agnostic**: Traverses the actual OSG skeleton hierarchy instead of using hardcoded bone names
- **Per-bone configuration**: Allows overrides for specific bones (e.g., tighter limits for spine, looser for shoulders)
- **Default heuristics**: Estimates bone shapes from parent-child distances when no config provided

### 2. RagdollWrapper (Replaces Current Ragdoll Class)

Wraps Jolt's ragdoll and handles OSG integration:

```cpp
class RagdollWrapper
{
public:
    RagdollWrapper(
        const MWWorld::Ptr& ptr,
        SceneUtil::Skeleton* skeleton,
        JPH::PhysicsSystem* joltSystem,
        const osg::Vec3f& initialVelocity
    );
    ~RagdollWrapper();

    // Sync physics bodies -> OSG bones (called after physics step)
    void updateBoneTransforms();

    // Apply impulse at world position
    void applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint);

    // Check if all bodies have come to rest
    bool isAtRest() const;

    // Activate all bodies
    void activate();

    // Get approximate center position
    osg::Vec3f getPosition() const;

private:
    MWWorld::Ptr mPtr;
    JPH::Ref<JPH::RagdollSettings> mSettings;
    JPH::Ragdoll* mRagdoll;  // Owned by physics system after AddToPhysicsSystem
    SceneUtil::Skeleton* mSkeleton;

    // Maps Jolt body index to OSG bone node
    struct BoneMapping
    {
        int joltBodyIndex;
        osg::MatrixTransform* osgNode;
        osg::Vec3f shapeOffset;  // Offset from joint to shape center
    };
    std::vector<BoneMapping> mBoneMappings;
};
```

### 3. Joint Configuration System

For future-proofing with different game rigs, we should support external configuration:

```yaml
# collision-shapes.yaml (existing) can be extended for ragdoll config

# Morrowind humanoid
bip01_ragdoll:
  - bone: "bip01 pelvis"
    mass: 0.15
    shape: box
  - bone: "bip01 spine"
    mass: 0.10
    shape: capsule
    swing_limit: 0.3
    twist_limit: 0.2
  - bone: "bip01 l upperarm"
    mass: 0.04
    shape: capsule
    swing_limit: 1.0  # Shoulder has wide range
    twist_limit: 1.0
  # ... etc

# Oblivion/Skyrim humanoid (future)
nif_humanoid_ragdoll:
  - bone: "Bip01 Pelvis"
    # ... different config
```

This allows modders to define ragdoll behavior for custom rigs.

## Implementation Steps

### Phase 1: Core Refactor
1. Create `RagdollSettingsBuilder` class
2. Create `RagdollWrapper` class using Jolt's built-in `Ragdoll`
3. Update `PhysicsSystem` to use new classes
4. Test with Morrowind skeletons

### Phase 2: Configuration System
1. Extend collision-shapes.yaml schema for ragdoll config
2. Add fallback heuristics for unconfigured skeletons
3. Test with various Morrowind creature rigs

### Phase 3: Future Game Support (Later)
1. Add Oblivion/Skyrim skeleton configurations
2. Handle NIF-specific bone naming conventions
3. Support for different skeleton types (beast races, creatures, etc.)

## Constraint Setup Details

The key insight from Jolt's documentation:

> "The assumption is that the body position/orientation IS the joint position/orientation. E.g. a capsule shape would be offset using a RotatedTranslatedShape so that it is correctly placed if you put the body on the joint."

This means:
1. Each `Part` body should be positioned at the **joint location** (bone origin)
2. The collision shape should be offset from that position using `RotatedTranslatedShape`
3. Constraint positions should be at the joint location (which is the body origin)
4. Use `EConstraintSpace::LocalToBodyCOM` for constraint settings

Example constraint setup:
```cpp
auto* constraint = new JPH::SwingTwistConstraintSettings;
constraint->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

// Position in each body's local space (at origin since body is at joint)
constraint->mPosition1 = JPH::Vec3::sZero();  // Parent body's joint point
constraint->mPosition2 = JPH::Vec3::sZero();  // Child body's joint point

// Axes in each body's local space
// These define the joint orientation relative to each body
constraint->mTwistAxis1 = parentLocalTwistAxis;
constraint->mTwistAxis2 = childLocalTwistAxis;
constraint->mPlaneAxis1 = parentLocalPlaneAxis;
constraint->mPlaneAxis2 = childLocalPlaneAxis;
```

## Bone Transform Sync

The `updateBoneTransforms()` method needs to:

1. Get each body's world transform from Jolt using `Ragdoll::GetPose()`
2. Convert from Jolt body position (at joint) to OSG bone matrix
3. Apply transforms in parent-to-child order to maintain hierarchy

```cpp
void RagdollWrapper::updateBoneTransforms()
{
    // Get current pose from Jolt
    JPH::SkeletonPose pose;
    pose.SetSkeleton(mSettings->GetSkeleton());
    mRagdoll->GetPose(pose);

    // Apply to OSG bones
    for (const auto& mapping : mBoneMappings)
    {
        const JPH::SkeletonPose::JointState& state = pose.GetJoint(mapping.joltBodyIndex);

        // Convert Jolt joint transform to OSG
        osg::Vec3f pos = toOsg(state.mTranslation);
        osg::Quat rot = toOsg(state.mRotation);

        // Calculate local transform relative to parent
        osg::Matrix parentWorldInv = getParentWorldMatrixInverse(mapping.osgNode);
        osg::Vec3f localPos = pos * parentWorldInv;
        osg::Quat localRot = parentWorldInv.getRotate().inverse() * rot;

        osg::Matrix localMatrix;
        localMatrix.makeRotate(localRot);
        localMatrix.setTrans(localPos);
        mapping.osgNode->setMatrix(localMatrix);
    }
}
```

## Collision Filtering

Jolt's `RagdollSettings::DisableParentChildCollisions()` automatically sets up collision groups so:
- Parent and child body parts don't collide with each other
- Ragdoll parts collide with the world
- Multiple ragdolls can collide with each other

We should also consider:
- Ragdoll parts should NOT collide with living actors (use DEBRIS layer)
- Ragdoll parts SHOULD collide with world geometry and heightfields

## Testing Checklist

- [ ] Ragdoll activates when actor dies
- [ ] Bones stay connected (constraints work)
- [ ] Ragdoll collides with ground and walls
- [ ] Ragdoll doesn't collide with living actors
- [ ] Hit impulse applies correctly
- [ ] Ragdoll comes to rest eventually
- [ ] Memory properly cleaned up when ragdoll removed
- [ ] Works with different Morrowind actor types (humanoid, creature)
- [ ] No visual artifacts (stretched vertices, disappearing mesh)
