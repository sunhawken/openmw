#ifndef OPENMW_MWPHYSICS_RAGDOLLWRAPPER_H
#define OPENMW_MWPHYSICS_RAGDOLLWRAPPER_H

#include "ragdollbuilder.hpp"

#include <osg/Vec3f>
#include <osg/Quat>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>

#include "../mwworld/ptr.hpp"

#include <memory>
#include <vector>
#include <string>

namespace JPH
{
    class PhysicsSystem;
}

namespace SceneUtil
{
    class Skeleton;
}

namespace MWPhysics
{
    class PhysicsTaskScheduler;

    /// Wrapper around Jolt's built-in Ragdoll class
    /// Handles creation from OSG skeleton and bone transform synchronization
    class RagdollWrapper
    {
    public:
        /// Create a ragdoll for a dead actor
        /// @param ptr The actor's MWWorld::Ptr
        /// @param skeleton The actor's OSG skeleton
        /// @param position Initial world position
        /// @param rotation Initial world rotation
        /// @param scale Actor scale factor
        /// @param joltSystem Jolt physics system
        /// @param scheduler Physics task scheduler for body interface access
        RagdollWrapper(
            const MWWorld::Ptr& ptr,
            SceneUtil::Skeleton* skeleton,
            const osg::Vec3f& position,
            const osg::Quat& rotation,
            float scale,
            JPH::PhysicsSystem* joltSystem,
            PhysicsTaskScheduler* scheduler
        );

        ~RagdollWrapper();

        RagdollWrapper(const RagdollWrapper&) = delete;
        RagdollWrapper& operator=(const RagdollWrapper&) = delete;

        /// Update the OSG skeleton's bone transforms from the physics simulation
        /// Should be called after physics step, before rendering
        void updateBoneTransforms();

        /// Apply an impulse to the ragdoll at a world position
        /// @param impulse The impulse vector in world space
        /// @param worldPoint The world-space point where the impulse is applied
        void applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint);

        /// Apply impulse to the root body (pelvis)
        void applyRootImpulse(const osg::Vec3f& impulse);

        /// Find the closest ragdoll body to a world position
        /// @param worldPoint The world position to search from
        /// @param outBodyIndex Output index of the closest body (-1 if no bodies)
        /// @return Distance to the closest body
        float findClosestBody(const osg::Vec3f& worldPoint, int& outBodyIndex) const;

        /// Get the world position of a specific body
        /// @param bodyIndex Index of the body (from findClosestBody)
        osg::Vec3f getBodyPosition(int bodyIndex) const;

        /// Set linear velocity of a specific body (for grabbing)
        /// @param bodyIndex Index of the body to move
        /// @param velocity Target velocity
        void setBodyVelocity(int bodyIndex, const osg::Vec3f& velocity);

        /// Set angular velocity of a specific body
        /// @param bodyIndex Index of the body
        /// @param angularVelocity Target angular velocity
        void setBodyAngularVelocity(int bodyIndex, const osg::Vec3f& angularVelocity);

        /// Get the number of bodies in this ragdoll
        int getBodyCount() const;

        /// Check if the ragdoll has come to rest (all bodies sleeping)
        bool isAtRest() const;

        /// Activate all bodies in the ragdoll
        void activate();

        /// Get the MWWorld::Ptr this ragdoll belongs to
        MWWorld::Ptr getPtr() const { return mPtr; }

        /// Update the ptr after cell change
        void updatePtr(const MWWorld::Ptr& ptr) { mPtr = ptr; }

        /// Get approximate center position of the ragdoll (root body position)
        osg::Vec3f getPosition() const;

        /// Get the root body ID (for collision queries)
        JPH::BodyID getRootBodyId() const;

        /// Get all body IDs in this ragdoll
        std::vector<JPH::BodyID> getBodyIds() const;

        /// Check if ragdoll was created successfully
        bool isValid() const { return mRagdoll != nullptr; }

    private:
        MWWorld::Ptr mPtr;
        SceneUtil::Skeleton* mSkeleton;
        JPH::PhysicsSystem* mJoltSystem;
        PhysicsTaskScheduler* mScheduler;

        JPH::Ref<JPH::RagdollSettings> mSettings;
        JPH::Ragdoll* mRagdoll;  // Owned by us, must call RemoveFromPhysicsSystem

        std::vector<BoneMapping> mBoneMappings;

        /// Collision group for this ragdoll (unique per ragdoll instance)
        JPH::CollisionGroup::GroupID mCollisionGroup;

        /// Next collision group ID
        static JPH::CollisionGroup::GroupID sNextCollisionGroup;
    };
}

#endif
