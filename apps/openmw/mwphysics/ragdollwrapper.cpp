#include "ragdollwrapper.hpp"

#include "joltlayers.hpp"
#include "mtphysics.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <vector>

namespace MWPhysics
{
    JPH::CollisionGroup::GroupID RagdollWrapper::sNextCollisionGroup = 1;

    RagdollWrapper::RagdollWrapper(
        const MWWorld::Ptr& ptr,
        SceneUtil::Skeleton* skeleton,
        const osg::Vec3f& position,
        const osg::Quat& rotation,
        float scale,
        JPH::PhysicsSystem* joltSystem,
        PhysicsTaskScheduler* scheduler)
        : mPtr(ptr)
        , mSkeleton(skeleton)
        , mJoltSystem(joltSystem)
        , mScheduler(scheduler)
        , mRagdoll(nullptr)
        , mCollisionGroup(sNextCollisionGroup++)
    {
        if (!skeleton || !joltSystem)
        {
            Log(Debug::Error) << "RagdollWrapper: Invalid skeleton or physics system";
            return;
        }

        // Build ragdoll settings from OSG skeleton
        const float totalMass = 70.0f * scale;
        mSettings = RagdollSettingsBuilder::build(skeleton, totalMass, scale);

        if (!mSettings)
        {
            Log(Debug::Error) << "RagdollWrapper: Failed to build ragdoll settings for "
                              << ptr.getCellRef().getRefId();
            return;
        }

        // Disable collisions between parent and child body parts
        mSettings->DisableParentChildCollisions(nullptr, true);

        // Create the ragdoll instance
        mRagdoll = mSettings->CreateRagdoll(mCollisionGroup, 0, joltSystem);

        if (!mRagdoll)
        {
            Log(Debug::Error) << "RagdollWrapper: Failed to create ragdoll instance for "
                              << ptr.getCellRef().getRefId();
            return;
        }

        // Add ragdoll to physics system first (bodies must exist before SetPose with BodyInterface)
        mRagdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

        // CRITICAL: Set the initial pose AFTER adding to physics system.
        // The low-level SetPose(RVec3, Mat44*) uses BodyInterface::SetPositionAndRotation
        // which requires bodies to be in the physics system.
        // Build pose from the RagdollSettings::Part positions/rotations.
        std::vector<JPH::Mat44> initialPose(mSettings->mParts.size());
        JPH::RVec3 rootOffset = mSettings->mParts[0].mPosition;
        for (size_t i = 0; i < mSettings->mParts.size(); ++i)
        {
            const auto& part = mSettings->mParts[i];
            // SetPose takes model-space matrices: translations relative to root offset, world rotations
            JPH::Vec3 relativePos = JPH::Vec3(part.mPosition - rootOffset);
            initialPose[i] = JPH::Mat44::sRotationTranslation(part.mRotation, relativePos);
        }
        mRagdoll->SetPose(rootOffset, initialPose.data(), true);  // Lock bodies for thread safety

        // Get skeleton root transform for the mapper
        osg::Matrix skeletonRootTransform;
        skeletonRootTransform.makeRotate(rotation);
        skeletonRootTransform.setTrans(position);

        // Initialize the skeleton mapper
        if (!mMapper.initialize(skeleton, mSettings->GetSkeleton(), mRagdoll, joltSystem, skeletonRootTransform))
        {
            Log(Debug::Error) << "RagdollWrapper: Failed to initialize skeleton mapper for "
                              << ptr.getCellRef().getRefId();
            // Continue anyway - ragdoll physics will work, just no visual sync
        }

        Log(Debug::Info) << "RagdollWrapper: Created ragdoll with "
                         << mRagdoll->GetBodyCount() << " bodies, "
                         << mRagdoll->GetConstraintCount() << " constraints"
                         << " for " << ptr.getCellRef().getRefId();
    }

    RagdollWrapper::~RagdollWrapper()
    {
        if (mRagdoll)
        {
            mRagdoll->RemoveFromPhysicsSystem();
            delete mRagdoll;
            mRagdoll = nullptr;
        }
    }

    void RagdollWrapper::updateBoneTransforms()
    {
        if (!mRagdoll || !mSkeleton)
            return;

        // Use the skeleton mapper to sync physics -> OSG
        if (mMapper.isValid())
        {
            mMapper.mapRagdollToOsg();
        }
    }

    void RagdollWrapper::applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint)
    {
        if (!mRagdoll)
            return;

        // Find the closest body to the world point
        int closestIndex = -1;
        float closestDist = findClosestBody(worldPoint, closestIndex);

        if (closestIndex >= 0)
        {
            JPH::BodyID bodyId = mRagdoll->GetBodyID(closestIndex);
            JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
            bodyInterface.AddImpulse(
                bodyId,
                Misc::Convert::toJolt<JPH::Vec3>(impulse),
                Misc::Convert::toJolt<JPH::RVec3>(worldPoint)
            );
        }
    }

    void RagdollWrapper::applyRootImpulse(const osg::Vec3f& impulse)
    {
        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return;

        JPH::BodyID rootId = mRagdoll->GetBodyID(0);
        if (!rootId.IsInvalid())
        {
            JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
            bodyInterface.AddImpulse(rootId, Misc::Convert::toJolt<JPH::Vec3>(impulse));
        }
    }

    bool RagdollWrapper::isAtRest() const
    {
        if (!mRagdoll)
            return true;

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();

        for (JPH::uint i = 0; i < mRagdoll->GetBodyCount(); ++i)
        {
            JPH::BodyID bodyId = mRagdoll->GetBodyID(i);
            if (!bodyId.IsInvalid() && bodyInterface.IsActive(bodyId))
                return false;
        }

        return true;
    }

    void RagdollWrapper::activate()
    {
        if (mRagdoll)
        {
            mRagdoll->Activate();
        }
    }

    osg::Vec3f RagdollWrapper::getPosition() const
    {
        if (mMapper.isValid())
            return mMapper.getRootPosition();

        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return osg::Vec3f();

        JPH::BodyID rootId = mRagdoll->GetBodyID(0);
        if (rootId.IsInvalid())
            return osg::Vec3f();

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        JPH::RVec3 rootPos = bodyInterface.GetCenterOfMassPosition(rootId);
        return Misc::Convert::toOsg(rootPos);
    }

    osg::Quat RagdollWrapper::getRotation() const
    {
        if (mMapper.isValid())
            return mMapper.getRootRotation();

        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return osg::Quat();

        JPH::BodyID rootId = mRagdoll->GetBodyID(0);
        if (rootId.IsInvalid())
            return osg::Quat();

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        return Misc::Convert::toOsg(bodyInterface.GetRotation(rootId));
    }

    JPH::BodyID RagdollWrapper::getRootBodyId() const
    {
        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return JPH::BodyID();

        return mRagdoll->GetBodyID(0);
    }

    std::vector<JPH::BodyID> RagdollWrapper::getBodyIds() const
    {
        std::vector<JPH::BodyID> ids;
        if (mRagdoll)
        {
            for (JPH::uint i = 0; i < mRagdoll->GetBodyCount(); ++i)
            {
                ids.push_back(mRagdoll->GetBodyID(i));
            }
        }
        return ids;
    }

    float RagdollWrapper::findClosestBody(const osg::Vec3f& worldPoint, int& outBodyIndex) const
    {
        outBodyIndex = -1;

        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return std::numeric_limits<float>::max();

        float closestDist = std::numeric_limits<float>::max();
        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();

        for (JPH::uint i = 0; i < mRagdoll->GetBodyCount(); ++i)
        {
            JPH::BodyID bodyId = mRagdoll->GetBodyID(i);
            if (bodyId.IsInvalid())
                continue;

            JPH::RVec3 bodyPos = bodyInterface.GetCenterOfMassPosition(bodyId);
            osg::Vec3f pos = Misc::Convert::toOsg(bodyPos);
            float dist = (pos - worldPoint).length();

            if (dist < closestDist)
            {
                closestDist = dist;
                outBodyIndex = static_cast<int>(i);
            }
        }

        return closestDist;
    }

    osg::Vec3f RagdollWrapper::getBodyPosition(int bodyIndex) const
    {
        if (!mRagdoll || bodyIndex < 0 || bodyIndex >= static_cast<int>(mRagdoll->GetBodyCount()))
            return osg::Vec3f();

        JPH::BodyID bodyId = mRagdoll->GetBodyID(bodyIndex);
        if (bodyId.IsInvalid())
            return osg::Vec3f();

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        JPH::RVec3 bodyPos = bodyInterface.GetCenterOfMassPosition(bodyId);
        return Misc::Convert::toOsg(bodyPos);
    }

    void RagdollWrapper::setBodyVelocity(int bodyIndex, const osg::Vec3f& velocity)
    {
        if (!mRagdoll || bodyIndex < 0 || bodyIndex >= static_cast<int>(mRagdoll->GetBodyCount()))
            return;

        JPH::BodyID bodyId = mRagdoll->GetBodyID(bodyIndex);
        if (bodyId.IsInvalid())
            return;

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        bodyInterface.SetLinearVelocity(bodyId, Misc::Convert::toJolt<JPH::Vec3>(velocity));
    }

    void RagdollWrapper::setBodyAngularVelocity(int bodyIndex, const osg::Vec3f& angularVelocity)
    {
        if (!mRagdoll || bodyIndex < 0 || bodyIndex >= static_cast<int>(mRagdoll->GetBodyCount()))
            return;

        JPH::BodyID bodyId = mRagdoll->GetBodyID(bodyIndex);
        if (bodyId.IsInvalid())
            return;

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        bodyInterface.SetAngularVelocity(bodyId, Misc::Convert::toJolt<JPH::Vec3>(angularVelocity));
    }

    int RagdollWrapper::getBodyCount() const
    {
        if (!mRagdoll)
            return 0;
        return static_cast<int>(mRagdoll->GetBodyCount());
    }
}
