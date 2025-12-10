#include "ragdollwrapper.hpp"

#include "joltlayers.hpp"
#include "mtphysics.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Skeleton/SkeletonPose.h>

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

        // Note: The OSG bone transforms should already reflect the current animation frame
        // since ragdoll activation happens during the mechanics update, after animations
        // have been processed. We read transforms directly from OSG nodes via getWorldMatrix()
        // which uses computeLocalToWorld(), not the skeleton's cached bone matrices.

        // Build ragdoll settings from OSG skeleton
        const float totalMass = 70.0f * scale;  // Roughly human weight
        mSettings = RagdollSettingsBuilder::build(
            skeleton,
            totalMass,
            scale,
            nullptr,  // No overrides for now
            mBoneMappings
        );

        if (!mSettings)
        {
            Log(Debug::Error) << "RagdollWrapper: Failed to build ragdoll settings for "
                              << ptr.getCellRef().getRefId();
            return;
        }

        // Disable collisions between parent and child body parts
        // This creates a GroupFilterTable that prevents adjacent parts from colliding
        // Pass nullptr for pose matrices to use the initial pose from settings
        // Set detectSeparations to true to warn if joints are too far apart
        mSettings->DisableParentChildCollisions(nullptr, true);

        // Create the ragdoll instance
        mRagdoll = mSettings->CreateRagdoll(mCollisionGroup, 0, joltSystem);

        if (!mRagdoll)
        {
            Log(Debug::Error) << "RagdollWrapper: Failed to create ragdoll instance for "
                              << ptr.getCellRef().getRefId();
            return;
        }

        // Add ragdoll to physics system and activate it
        mRagdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

        Log(Debug::Info) << "RagdollWrapper: Created ragdoll with " << mBoneMappings.size()
                         << " bodies, " << mRagdoll->GetConstraintCount() << " constraints"
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
        if (!mRagdoll || !mSkeleton || mBoneMappings.empty())
            return;

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();

        // STEP 1: Collect all physics world transforms first
        // We must do this BEFORE modifying any OSG nodes, because modifying a parent
        // would affect the world matrix computation of its children
        struct PhysicsTransform
        {
            osg::Vec3f worldPos;
            osg::Quat worldRot;
            bool valid;
        };
        std::vector<PhysicsTransform> physicsTransforms(mBoneMappings.size());

        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            const BoneMapping& mapping = mBoneMappings[i];
            physicsTransforms[i].valid = false;

            if (!mapping.osgNode)
                continue;

            if (mapping.joltJointIndex >= static_cast<int>(mRagdoll->GetBodyCount()))
                continue;

            JPH::BodyID bodyId = mRagdoll->GetBodyID(mapping.joltJointIndex);
            if (bodyId.IsInvalid())
                continue;

            JPH::RVec3 physicsPos;
            JPH::Quat physicsRot;
            bodyInterface.GetPositionAndRotation(bodyId, physicsPos, physicsRot);

            physicsTransforms[i].worldPos = Misc::Convert::toOsg(physicsPos);
            physicsTransforms[i].worldRot = Misc::Convert::toOsg(physicsRot);
            physicsTransforms[i].valid = true;
        }

        // STEP 2: Build a map from bone name to physics transform for parent lookup
        std::unordered_map<std::string, size_t> boneNameToIndex;
        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (physicsTransforms[i].valid)
            {
                std::string lowerName = Misc::StringUtils::lowerCase(mBoneMappings[i].boneName);
                boneNameToIndex[lowerName] = i;
            }
        }

        // STEP 3: Apply transforms
        // IMPORTANT: We must compute local matrix relative to OSG parent (not physics parent)
        // because that's what setMatrix() expects. But we use physics transforms for world positions.
        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (!physicsTransforms[i].valid)
                continue;

            const BoneMapping& mapping = mBoneMappings[i];
            const osg::Vec3f& worldPos = physicsTransforms[i].worldPos;
            const osg::Quat& worldRot = physicsTransforms[i].worldRot;

            // Get the OSG parent's world transform
            // If OSG parent is a physics bone, use its physics world transform
            // Otherwise use the OSG computed world transform
            osg::Vec3f osgParentWorldPos(0, 0, 0);
            osg::Quat osgParentWorldRot;  // Identity
            float parentScale = 1.0f;
            bool foundParentTransform = false;

            osg::Node* osgParent = mapping.osgNode->getParent(0);
            if (osgParent)
            {
                // Check if OSG parent is a physics bone
                auto* parentTransform = dynamic_cast<osg::MatrixTransform*>(osgParent);
                if (parentTransform && !parentTransform->getName().empty())
                {
                    std::string osgParentLowerName = Misc::StringUtils::lowerCase(parentTransform->getName());
                    auto parentIt = boneNameToIndex.find(osgParentLowerName);
                    if (parentIt != boneNameToIndex.end() && physicsTransforms[parentIt->second].valid)
                    {
                        // OSG parent is a physics bone - use its physics world transform
                        osgParentWorldPos = physicsTransforms[parentIt->second].worldPos;
                        osgParentWorldRot = physicsTransforms[parentIt->second].worldRot;
                        foundParentTransform = true;

                        // Get parent scale from NifOsg::MatrixTransform if possible
                        auto* nifParent = dynamic_cast<NifOsg::MatrixTransform*>(parentTransform);
                        if (nifParent && nifParent->mScale != 0.0f)
                            parentScale = nifParent->mScale;
                    }
                }

                // If OSG parent is not a physics bone, use OSG transform
                if (!foundParentTransform)
                {
                    // Note: getParentalNodePaths() returns paths TO the node, not INCLUDING it
                    // So we need to also include the parent's own transform
                    osg::NodePathList nodePaths = osgParent->getParentalNodePaths();
                    if (!nodePaths.empty())
                    {
                        // Add the parent itself to the path to get complete world transform
                        osg::NodePath pathWithParent = nodePaths[0];
                        pathWithParent.push_back(osgParent);
                        osg::Matrix parentWorld = osg::computeLocalToWorld(pathWithParent);
                        osgParentWorldPos = parentWorld.getTrans();
                        osgParentWorldRot = parentWorld.getRotate();
                        foundParentTransform = true;

                        // Get scale from parent if it's a NifOsg::MatrixTransform
                        auto* nifParent = dynamic_cast<NifOsg::MatrixTransform*>(osgParent);
                        if (nifParent && nifParent->mScale != 0.0f)
                            parentScale = nifParent->mScale;
                    }
                }
            }

            // If we still don't have a parent transform (shouldn't happen), fall back to
            // using the bone's own node paths to get the transform of everything above it
            if (!foundParentTransform)
            {
                osg::NodePathList nodePaths = mapping.osgNode->getParentalNodePaths();
                if (!nodePaths.empty())
                {
                    // Don't include the bone itself - just get transform up to it
                    osg::Matrix parentWorld = osg::computeLocalToWorld(nodePaths[0]);
                    osgParentWorldPos = parentWorld.getTrans();
                    osgParentWorldRot = parentWorld.getRotate();
                }
            }

            // Compute local transform relative to OSG parent
            // localPos = inverse(osgParentWorldRot) * (worldPos - osgParentWorldPos)
            osg::Vec3f localPos = osgParentWorldRot.inverse() * (worldPos - osgParentWorldPos);

            // Account for parent's scale in position calculation
            if (parentScale != 1.0f && parentScale != 0.0f)
                localPos /= parentScale;

            // localRot = inverse(osgParentWorldRot) * worldRot
            osg::Quat localRot = osgParentWorldRot.inverse() * worldRot;

            // CRITICAL: Use NifOsg::MatrixTransform's setRotation/setTranslation methods
            // instead of setMatrix(). NifOsg::MatrixTransform stores mScale and mRotationScale
            // separately from the matrix. Using setMatrix() would bypass those stored components,
            // causing the scale to be lost and animations/other systems that read mScale to
            // get incorrect values. setRotation() and setTranslation() properly preserve the
            // scale component.
            auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(mapping.osgNode);
            if (nifTransform)
            {
                // Use the NifOsg-specific methods that preserve scale
                nifTransform->setRotation(localRot);
                nifTransform->setTranslation(localPos);
            }
            else
            {
                // Fallback for regular osg::MatrixTransform (shouldn't happen for NIF bones)
                osg::Matrix localMatrix;
                localMatrix.makeRotate(localRot);
                localMatrix.setTrans(localPos);
                mapping.osgNode->setMatrix(localMatrix);
            }
        }
    }

    void RagdollWrapper::applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint)
    {
        if (!mRagdoll || mBoneMappings.empty())
            return;

        // Find the closest body to the world point
        float closestDist = std::numeric_limits<float>::max();
        int closestIndex = -1;

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();

        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (mBoneMappings[i].joltJointIndex >= static_cast<int>(mRagdoll->GetBodyCount()))
                continue;

            JPH::BodyID bodyId = mRagdoll->GetBodyID(mBoneMappings[i].joltJointIndex);
            if (bodyId.IsInvalid())
                continue;

            JPH::RVec3 bodyPos = bodyInterface.GetCenterOfMassPosition(bodyId);
            osg::Vec3f pos = Misc::Convert::toOsg(bodyPos);
            float dist = (pos - worldPoint).length2();

            if (dist < closestDist)
            {
                closestDist = dist;
                closestIndex = static_cast<int>(i);
            }
        }

        if (closestIndex >= 0)
        {
            JPH::BodyID bodyId = mRagdoll->GetBodyID(mBoneMappings[closestIndex].joltJointIndex);
            bodyInterface.AddImpulse(
                bodyId,
                Misc::Convert::toJolt<JPH::Vec3>(impulse),
                Misc::Convert::toJolt<JPH::RVec3>(worldPoint)
            );
        }
    }

    void RagdollWrapper::applyRootImpulse(const osg::Vec3f& impulse)
    {
        if (!mRagdoll || mBoneMappings.empty())
            return;

        // Apply to first body (root)
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
        if (!mRagdoll || mRagdoll->GetBodyCount() == 0)
            return osg::Vec3f();

        // Get root body position directly from physics system
        JPH::BodyID rootId = mRagdoll->GetBodyID(0);
        if (rootId.IsInvalid())
            return osg::Vec3f();

        JPH::BodyInterface& bodyInterface = mJoltSystem->GetBodyInterface();
        JPH::RVec3 rootPos = bodyInterface.GetCenterOfMassPosition(rootId);
        return Misc::Convert::toOsg(rootPos);
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
