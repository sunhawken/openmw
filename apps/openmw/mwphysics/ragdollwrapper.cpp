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

        static int debugCounter = 0;
        bool doDebug = (debugCounter++ % 300 == 0);  // Log every ~5 seconds at 60fps

        // STEP 1: Collect all physics world transforms first
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

        // STEP 2: Find the root physics bone (pelvis) and move its OSG parent (Bip01) to follow it
        // This is critical: the mesh is attached to the entire skeleton hierarchy including Bip01.
        // If we don't move Bip01, the mesh stays at the death position while bones move relative to it.
        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (!physicsTransforms[i].valid)
                continue;

            const BoneMapping& mapping = mBoneMappings[i];

            // Check if this is the root physics bone (no physics parent)
            if (mapping.physicsParentName.empty())
            {
                const osg::Vec3f& rootPhysicsWorldPos = physicsTransforms[i].worldPos;
                const osg::Quat& rootPhysicsWorldRot = physicsTransforms[i].worldRot;

                // Get the OSG parent of the root physics bone (e.g., Bip01 for pelvis)
                osg::Node* osgParent = mapping.osgNode->getParent(0);
                auto* bip01 = dynamic_cast<osg::MatrixTransform*>(osgParent);
                if (bip01)
                {
                    // Get Bip01's parent world transform (the skeleton root / actor transform)
                    osg::Matrix bip01ParentWorld;
                    osg::Node* bip01Parent = bip01->getParent(0);
                    if (bip01Parent)
                    {
                        osg::NodePathList nodePaths = bip01Parent->getParentalNodePaths();
                        if (!nodePaths.empty())
                        {
                            osg::NodePath pathWithParent = nodePaths[0];
                            pathWithParent.push_back(bip01Parent);
                            bip01ParentWorld = osg::computeLocalToWorld(pathWithParent);
                        }
                    }

                    osg::Vec3f bip01ParentWorldPos = bip01ParentWorld.getTrans();
                    osg::Quat bip01ParentWorldRot = bip01ParentWorld.getRotate();

                    // Get the original local offset from Bip01 to pelvis (from the original bone setup)
                    // This is stored in the pelvis bone's original local transform
                    osg::Matrix pelvisOriginalLocal = mapping.osgNode->getMatrix();
                    osg::Vec3f pelvisLocalOffset = pelvisOriginalLocal.getTrans();

                    // We want: pelvis ends up at rootPhysicsWorldPos
                    // pelvisWorld = pelvisLocal * bip01World
                    // pelvisWorld = pelvisLocal * (bip01Local * bip01ParentWorld)
                    // So: bip01World = pelvisWorld * inverse(pelvisLocal)
                    // And: bip01Local = bip01World * inverse(bip01ParentWorld)

                    // For simplicity, we'll move Bip01 so that pelvis (with its original local offset)
                    // ends up at the physics position.
                    //
                    // The pelvis physics body is at rootPhysicsWorldPos with rotation rootPhysicsWorldRot.
                    // The pelvis bone has a local offset (pelvisLocalOffset) from Bip01.
                    // We need to find where Bip01 should be so that:
                    //   rootPhysicsWorldPos = pelvisLocalOffset * bip01WorldRot + bip01WorldPos
                    //
                    // Solving for bip01WorldPos:
                    //   bip01WorldPos = rootPhysicsWorldPos - pelvisLocalOffset * bip01WorldRot
                    //
                    // For rotation, we use the physics rotation directly for Bip01,
                    // and pelvis will inherit it (pelvis local rot becomes identity-ish)

                    // Use physics rotation for Bip01
                    osg::Quat bip01WorldRot = rootPhysicsWorldRot;

                    // Calculate where Bip01 needs to be in world space
                    osg::Matrix bip01RotMat;
                    bip01RotMat.makeRotate(bip01WorldRot);
                    osg::Vec3f rotatedOffset = pelvisLocalOffset * bip01RotMat;
                    osg::Vec3f bip01WorldPos = rootPhysicsWorldPos - rotatedOffset;

                    // Convert Bip01's desired world transform to local (relative to its parent)
                    osg::Vec3f bip01DeltaPos = bip01WorldPos - bip01ParentWorldPos;
                    osg::Matrix invBip01ParentRot;
                    invBip01ParentRot.makeRotate(bip01ParentWorldRot.inverse());
                    osg::Vec3f bip01LocalPos = bip01DeltaPos * invBip01ParentRot;
                    osg::Quat bip01LocalRot = bip01WorldRot * bip01ParentWorldRot.inverse();

                    // Apply to Bip01
                    auto* nifBip01 = dynamic_cast<NifOsg::MatrixTransform*>(bip01);
                    if (nifBip01)
                    {
                        nifBip01->setRotation(bip01LocalRot);
                        nifBip01->setTranslation(bip01LocalPos);
                    }
                    else
                    {
                        osg::Matrix bip01LocalMatrix;
                        bip01LocalMatrix.makeRotate(bip01LocalRot);
                        bip01LocalMatrix.setTrans(bip01LocalPos);
                        bip01->setMatrix(bip01LocalMatrix);
                    }

                    if (doDebug)
                    {
                        Log(Debug::Info) << "RAGDOLL DEBUG: Moved Bip01 to worldPos=("
                            << bip01WorldPos.x() << ", " << bip01WorldPos.y() << ", " << bip01WorldPos.z() << ")"
                            << " so pelvis at physicsPos=(" << rootPhysicsWorldPos.x() << ", "
                            << rootPhysicsWorldPos.y() << ", " << rootPhysicsWorldPos.z() << ")";
                    }
                }
                break;  // Only process root once
            }
        }

        // STEP 3: Build a map from bone name to physics transform for parent lookup
        std::unordered_map<std::string, size_t> boneNameToIndex;
        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (physicsTransforms[i].valid)
            {
                std::string lowerName = Misc::StringUtils::lowerCase(mBoneMappings[i].boneName);
                boneNameToIndex[lowerName] = i;
            }
        }

        // STEP 4: Apply transforms for all non-root physics bones
        // Now that Bip01 has moved, we compute local transforms relative to physics parents
        for (size_t i = 0; i < mBoneMappings.size(); ++i)
        {
            if (!physicsTransforms[i].valid)
                continue;

            const BoneMapping& mapping = mBoneMappings[i];

            // Skip root - it was handled by moving Bip01
            if (mapping.physicsParentName.empty())
                continue;

            const osg::Vec3f& worldPos = physicsTransforms[i].worldPos;
            const osg::Quat& worldRot = physicsTransforms[i].worldRot;

            // Find physics parent's world transform
            osg::Vec3f parentWorldPos(0, 0, 0);
            osg::Quat parentWorldRot;

            std::string parentLowerName = Misc::StringUtils::lowerCase(mapping.physicsParentName);
            auto parentIt = boneNameToIndex.find(parentLowerName);
            if (parentIt != boneNameToIndex.end() && physicsTransforms[parentIt->second].valid)
            {
                parentWorldPos = physicsTransforms[parentIt->second].worldPos;
                parentWorldRot = physicsTransforms[parentIt->second].worldRot;
            }
            else
            {
                // Fallback: shouldn't happen if physics hierarchy is correct
                Log(Debug::Warning) << "RAGDOLL: Could not find physics parent " << mapping.physicsParentName
                                    << " for " << mapping.boneName;
                continue;
            }

            // Compute local transform relative to physics parent
            osg::Vec3f deltaPos = worldPos - parentWorldPos;
            osg::Matrix invParentRot;
            invParentRot.makeRotate(parentWorldRot.inverse());
            osg::Vec3f localPos = deltaPos * invParentRot;

            osg::Quat localRot = worldRot * parentWorldRot.inverse();

            // Apply to OSG node
            auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(mapping.osgNode);
            if (nifTransform)
            {
                nifTransform->setRotation(localRot);
                nifTransform->setTranslation(localPos);
            }
            else
            {
                osg::Matrix localMatrix;
                localMatrix.makeRotate(localRot);
                localMatrix.setTrans(localPos);
                mapping.osgNode->setMatrix(localMatrix);
            }

            if (doDebug && mapping.boneName == "bip01 spine")
            {
                Log(Debug::Info) << "RAGDOLL DEBUG [spine]: physicsWorldPos=("
                    << worldPos.x() << ", " << worldPos.y() << ", " << worldPos.z() << ")"
                    << " parentWorldPos=(" << parentWorldPos.x() << ", " << parentWorldPos.y() << ", " << parentWorldPos.z() << ")"
                    << " localPos=(" << localPos.x() << ", " << localPos.y() << ", " << localPos.z() << ")";
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
