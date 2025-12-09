#ifndef OPENMW_MWPHYSICS_RAGDOLL_H
#define OPENMW_MWPHYSICS_RAGDOLL_H

#include "ptrholder.hpp"

#include <osg/Vec3f>
#include <osg/Quat>
#include <osg/MatrixTransform>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <memory>
#include <vector>
#include <string>

namespace SceneUtil
{
    class Skeleton;
    class Bone;
}

namespace MWPhysics
{
    class PhysicsTaskScheduler;
    class PhysicsSystem;

    // Represents a single bone in the ragdoll with its physics body and rendering node
    struct RagdollBone
    {
        std::string name;
        int partIndex;  // Index in JPH::Ragdoll
        osg::MatrixTransform* node;  // The bone's scene graph node
        osg::Vec3f localOffset;  // Offset from bone origin to shape center
    };

    // A physics-driven ragdoll for dead NPCs/creatures
    // Replaces the kinematic Actor body with multiple dynamic bodies connected by constraints
    class Ragdoll
    {
    public:
        Ragdoll(const MWWorld::Ptr& ptr,
                SceneUtil::Skeleton* skeleton,
                const osg::Vec3f& position,
                const osg::Quat& rotation,
                float scale,
                PhysicsTaskScheduler* scheduler,
                PhysicsSystem* physicsSystem);
        ~Ragdoll();

        Ragdoll(const Ragdoll&) = delete;
        Ragdoll& operator=(const Ragdoll&) = delete;

        // Update the skeleton's bone transforms from the physics simulation
        // Should be called after physics step, before rendering
        void updateBoneTransforms();

        // Apply an impulse to the ragdoll (e.g., from the killing blow)
        // @param impulse The impulse vector
        // @param worldPoint The world-space point where the impulse is applied
        void applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint);

        // Apply impulse to the root body (pelvis)
        void applyRootImpulse(const osg::Vec3f& impulse);

        // Check if the ragdoll has come to rest
        bool isAtRest() const;

        // Activate all bodies in the ragdoll
        void activate();

        // Get the MWWorld::Ptr this ragdoll belongs to
        MWWorld::Ptr getPtr() const { return mPtr; }

        // Update the ptr after cell change
        void updatePtr(const MWWorld::Ptr& ptr) { mPtr = ptr; }

        // Get approximate center position of the ragdoll (pelvis position)
        osg::Vec3f getPosition() const;

        // Get the root body ID (for collision queries)
        JPH::BodyID getRootBodyId() const;

        // Get all body IDs in this ragdoll
        const std::vector<JPH::BodyID>& getBodyIds() const { return mBodyIds; }

    private:
        // Find a bone by name in the skeleton
        osg::MatrixTransform* findBone(SceneUtil::Skeleton* skeleton, const std::string& name);

        // Calculate bone dimensions from the skeleton
        osg::Vec3f estimateBoneSize(osg::MatrixTransform* bone, osg::MatrixTransform* childBone);

        MWWorld::Ptr mPtr;
        std::vector<RagdollBone> mBones;
        std::vector<JPH::BodyID> mBodyIds;
        PhysicsTaskScheduler* mTaskScheduler;
        PhysicsSystem* mPhysicsSystem;
        SceneUtil::Skeleton* mSkeleton;
        osg::Vec3f mRootOffset;  // Offset from actor position to root bone
    };
}

#endif
