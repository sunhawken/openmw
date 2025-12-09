#include "ragdoll.hpp"

#include "joltlayers.hpp"
#include "mtphysics.hpp"
#include "physicssystem.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/physicshelpers/collisionobject.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <osg/NodeVisitor>
#include <osg/Node>

#include <algorithm>
#include <cmath>

namespace MWPhysics
{
    // Bone definition for ragdoll construction
    struct BoneDef
    {
        const char* name;
        const char* parentName;  // nullptr for root
        float mass;  // Relative mass
        enum ShapeType { Capsule, Box, Sphere } shapeType;
        // Joint limits (in radians)
        float twistMin, twistMax;
        float swingY, swingZ;
    };

    // Standard humanoid bone definitions
    // These match the Bip01 skeleton used in Morrowind
    static const BoneDef sHumanoidBones[] = {
        // Root/Pelvis - no parent
        { "bip01 pelvis", nullptr, 0.15f, BoneDef::Box, 0, 0, 0, 0 },

        // Spine chain
        { "bip01 spine", "bip01 pelvis", 0.10f, BoneDef::Capsule,
          -0.3f, 0.3f, 0.4f, 0.3f },  // Limited twist and bend
        { "bip01 spine1", "bip01 spine", 0.10f, BoneDef::Capsule,
          -0.2f, 0.2f, 0.3f, 0.2f },
        { "bip01 spine2", "bip01 spine1", 0.10f, BoneDef::Capsule,
          -0.2f, 0.2f, 0.3f, 0.2f },

        // Neck and head
        { "bip01 neck", "bip01 spine2", 0.03f, BoneDef::Capsule,
          -0.5f, 0.5f, 0.5f, 0.5f },  // Neck can rotate more
        { "bip01 head", "bip01 neck", 0.08f, BoneDef::Sphere,
          -0.3f, 0.3f, 0.4f, 0.3f },

        // Left arm
        { "bip01 l clavicle", "bip01 spine2", 0.02f, BoneDef::Capsule,
          -0.2f, 0.2f, 0.3f, 0.3f },
        { "bip01 l upperarm", "bip01 l clavicle", 0.04f, BoneDef::Capsule,
          -1.5f, 1.5f, 1.2f, 1.2f },  // Shoulder has wide range
        { "bip01 l forearm", "bip01 l upperarm", 0.03f, BoneDef::Capsule,
          -0.1f, 2.5f, 0.1f, 0.1f },  // Elbow mainly bends one way
        { "bip01 l hand", "bip01 l forearm", 0.01f, BoneDef::Box,
          -0.5f, 0.5f, 0.8f, 0.3f },

        // Right arm
        { "bip01 r clavicle", "bip01 spine2", 0.02f, BoneDef::Capsule,
          -0.2f, 0.2f, 0.3f, 0.3f },
        { "bip01 r upperarm", "bip01 r clavicle", 0.04f, BoneDef::Capsule,
          -1.5f, 1.5f, 1.2f, 1.2f },
        { "bip01 r forearm", "bip01 r upperarm", 0.03f, BoneDef::Capsule,
          -0.1f, 2.5f, 0.1f, 0.1f },
        { "bip01 r hand", "bip01 r forearm", 0.01f, BoneDef::Box,
          -0.5f, 0.5f, 0.8f, 0.3f },

        // Left leg
        { "bip01 l thigh", "bip01 pelvis", 0.07f, BoneDef::Capsule,
          -0.5f, 0.5f, 1.2f, 0.3f },  // Hip has good range
        { "bip01 l calf", "bip01 l thigh", 0.05f, BoneDef::Capsule,
          -0.1f, 0.1f, 2.5f, 0.1f },  // Knee mainly bends backward
        { "bip01 l foot", "bip01 l calf", 0.02f, BoneDef::Box,
          -0.3f, 0.3f, 0.5f, 0.3f },

        // Right leg
        { "bip01 r thigh", "bip01 pelvis", 0.07f, BoneDef::Capsule,
          -0.5f, 0.5f, 1.2f, 0.3f },
        { "bip01 r calf", "bip01 r thigh", 0.05f, BoneDef::Capsule,
          -0.1f, 0.1f, 2.5f, 0.1f },
        { "bip01 r foot", "bip01 r calf", 0.02f, BoneDef::Box,
          -0.3f, 0.3f, 0.5f, 0.3f },
    };
    static const int sNumHumanoidBones = sizeof(sHumanoidBones) / sizeof(sHumanoidBones[0]);

    // Visitor to find a bone by name in the scene graph
    class FindBoneVisitor : public osg::NodeVisitor
    {
    public:
        FindBoneVisitor(const std::string& name)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mName(Misc::StringUtils::lowerCase(name))
            , mFound(nullptr)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            if (Misc::StringUtils::lowerCase(node.getName()) == mName)
            {
                mFound = &node;
                return;  // Stop traversal
            }
            traverse(node);
        }

        osg::MatrixTransform* getFound() { return mFound; }

    private:
        std::string mName;
        osg::MatrixTransform* mFound;
    };

    // Helper to get world matrix for a node
    static osg::Matrix getWorldMatrix(osg::Node* node)
    {
        osg::Matrix worldMatrix;
        if (!node)
            return worldMatrix;

        osg::NodePathList nodePaths = node->getParentalNodePaths();
        if (!nodePaths.empty())
        {
            worldMatrix = osg::computeLocalToWorld(nodePaths[0]);
        }
        return worldMatrix;
    }

    Ragdoll::Ragdoll(const MWWorld::Ptr& ptr,
                     SceneUtil::Skeleton* skeleton,
                     const osg::Vec3f& position,
                     const osg::Quat& rotation,
                     float scale,
                     PhysicsTaskScheduler* scheduler,
                     PhysicsSystem* physicsSystem)
        : mPtr(ptr)
        , mTaskScheduler(scheduler)
        , mPhysicsSystem(physicsSystem)
        , mSkeleton(skeleton)
        , mRootOffset(0, 0, 0)
    {
        if (!skeleton)
        {
            Log(Debug::Error) << "Ragdoll: Cannot create ragdoll without skeleton for "
                              << ptr.getCellRef().getRefId();
            return;
        }

        // Total mass for the ragdoll (in kg, roughly human weight)
        const float totalMass = 70.0f * scale;

        // Build the ragdoll body parts
        std::vector<JPH::BodyCreationSettings> bodySettings;

        for (int i = 0; i < sNumHumanoidBones; ++i)
        {
            const BoneDef& boneDef = sHumanoidBones[i];

            osg::MatrixTransform* boneNode = findBone(skeleton, boneDef.name);
            if (!boneNode)
            {
                Log(Debug::Verbose) << "Ragdoll: Bone not found: " << boneDef.name;
                continue;
            }

            // Get bone's world transform
            osg::Matrix boneWorldMatrix = getWorldMatrix(boneNode);
            osg::Vec3f boneWorldPos = boneWorldMatrix.getTrans();
            osg::Quat boneWorldRot = boneWorldMatrix.getRotate();

            // Estimate bone dimensions
            osg::Vec3f boneSize(10.0f * scale, 10.0f * scale, 20.0f * scale);  // Default size

            // Try to find child bone for better size estimation
            if (i + 1 < sNumHumanoidBones && sHumanoidBones[i + 1].parentName &&
                std::string(sHumanoidBones[i + 1].parentName) == boneDef.name)
            {
                osg::MatrixTransform* childBone = findBone(skeleton, sHumanoidBones[i + 1].name);
                if (childBone)
                {
                    boneSize = estimateBoneSize(boneNode, childBone);
                }
            }

            // Ensure minimum size
            boneSize.x() = std::max(boneSize.x(), 5.0f * scale);
            boneSize.y() = std::max(boneSize.y(), 5.0f * scale);
            boneSize.z() = std::max(boneSize.z(), 10.0f * scale);

            // Create appropriate shape
            JPH::Ref<JPH::Shape> shape;
            osg::Vec3f shapeOffset(0, 0, 0);

            switch (boneDef.shapeType)
            {
                case BoneDef::Sphere:
                {
                    float radius = std::max({boneSize.x(), boneSize.y(), boneSize.z()}) * 0.5f;
                    shape = new JPH::SphereShape(radius);
                    break;
                }
                case BoneDef::Box:
                {
                    shape = new JPH::BoxShape(JPH::Vec3(boneSize.x() * 0.5f, boneSize.y() * 0.5f, boneSize.z() * 0.5f));
                    shapeOffset = osg::Vec3f(0, 0, boneSize.z() * 0.5f);
                    break;
                }
                case BoneDef::Capsule:
                default:
                {
                    // Capsule along bone direction (Z axis)
                    float radius = std::min(boneSize.x(), boneSize.y()) * 0.4f;
                    float halfHeight = boneSize.z() * 0.5f - radius;
                    if (halfHeight < 0.0f)
                    {
                        halfHeight = 0.0f;
                        radius = boneSize.z() * 0.5f;
                    }
                    // Jolt capsules are along Y by default, rotate to Z
                    JPH::Quat capsuleRot = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), JPH::JPH_PI * 0.5f);
                    shape = new JPH::RotatedTranslatedShape(
                        JPH::Vec3(0, 0, halfHeight + radius),
                        capsuleRot,
                        new JPH::CapsuleShape(halfHeight, radius));
                    shapeOffset = osg::Vec3f(0, 0, halfHeight + radius);
                    break;
                }
            }

            // Create body settings
            float partMass = totalMass * boneDef.mass;

            // Calculate world position for the body
            osg::Vec3f bodyWorldPos = boneWorldPos + boneWorldRot * shapeOffset;

            JPH::BodyCreationSettings settings(
                shape,
                Misc::Convert::toJolt<JPH::RVec3>(bodyWorldPos),
                Misc::Convert::toJolt(boneWorldRot),
                JPH::EMotionType::Dynamic,
                Layers::DEBRIS);  // Use DEBRIS layer - no collision with actors

            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = partMass;
            settings.mLinearDamping = 0.1f;
            settings.mAngularDamping = 0.3f;
            settings.mFriction = 0.5f;
            settings.mRestitution = 0.1f;
            settings.mMotionQuality = JPH::EMotionQuality::LinearCast;
            settings.mAllowSleeping = true;
            settings.mGravityFactor = 1.0f;

            int partIndex = static_cast<int>(bodySettings.size());
            bodySettings.push_back(settings);

            // Store bone mapping for later updates
            RagdollBone ragdollBone;
            ragdollBone.name = boneDef.name;
            ragdollBone.partIndex = partIndex;
            ragdollBone.node = boneNode;
            ragdollBone.localOffset = shapeOffset;
            mBones.push_back(ragdollBone);

            // Store root offset
            if (!boneDef.parentName)
            {
                mRootOffset = bodyWorldPos - position;
            }
        }

        if (bodySettings.empty())
        {
            Log(Debug::Warning) << "Ragdoll: No bones found for " << ptr.getCellRef().getRefId();
            return;
        }

        // Create all bodies
        for (auto& settings : bodySettings)
        {
            JPH::Body* body = mTaskScheduler->createPhysicsBody(settings);
            if (body)
            {
                body->SetUserData(0);  // Don't associate with PtrHolder
                mTaskScheduler->addCollisionObject(body, true);
                mBodyIds.push_back(body->GetID());
            }
        }

        // TODO: Add constraints between bodies for proper ragdoll behavior
        // For now, bodies are independent - this creates a "loose" ragdoll
        // To add constraints, we need access to JPH::PhysicsSystem::AddConstraint()

        Log(Debug::Info) << "Ragdoll: Created ragdoll with " << mBodyIds.size()
                         << " bodies for " << ptr.getCellRef().getRefId();
    }

    Ragdoll::~Ragdoll()
    {
        // Remove all bodies
        for (const JPH::BodyID& bodyId : mBodyIds)
        {
            JPH::BodyLockWrite lock(mTaskScheduler->getBodyLockInterface(), bodyId);
            if (lock.Succeeded())
            {
                JPH::Body& body = lock.GetBody();
                mTaskScheduler->removeCollisionObject(&body);
                mTaskScheduler->destroyCollisionObject(&body);
            }
        }
        mBodyIds.clear();
    }

    osg::MatrixTransform* Ragdoll::findBone(SceneUtil::Skeleton* skeleton, const std::string& name)
    {
        FindBoneVisitor visitor(name);
        skeleton->accept(visitor);
        return visitor.getFound();
    }

    osg::Vec3f Ragdoll::estimateBoneSize(osg::MatrixTransform* bone, osg::MatrixTransform* childBone)
    {
        // Calculate distance from bone to child as the length
        osg::Matrix boneWorld = getWorldMatrix(bone);
        osg::Matrix childWorld = getWorldMatrix(childBone);

        osg::Vec3f bonePos = boneWorld.getTrans();
        osg::Vec3f childPos = childWorld.getTrans();

        float length = (childPos - bonePos).length();

        // Estimate width based on typical proportions
        float width = length * 0.3f;

        return osg::Vec3f(width, width, length);
    }

    void Ragdoll::updateBoneTransforms()
    {
        if (!mSkeleton || mBones.empty())
            return;

        for (const RagdollBone& bone : mBones)
        {
            if (bone.partIndex >= static_cast<int>(mBodyIds.size()) || !bone.node)
                continue;

            JPH::BodyID bodyId = mBodyIds[bone.partIndex];
            JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), bodyId);

            if (!lock.Succeeded())
                continue;

            const JPH::Body& body = lock.GetBody();

            // Get body transform (RVec3 for position)
            JPH::RVec3 bodyPos = body.GetCenterOfMassPosition();
            JPH::Quat bodyRot = body.GetRotation();

            // Convert to OSG
            osg::Vec3f worldPos = Misc::Convert::toOsg(bodyPos);
            osg::Quat worldRot = Misc::Convert::toOsg(bodyRot);

            // Calculate local transform relative to parent
            osg::Node* parent = bone.node->getParent(0);
            if (parent)
            {
                osg::Matrix parentWorldInv = osg::Matrix::inverse(getWorldMatrix(parent));

                // Transform world position to parent space
                osg::Vec3f localPos = worldPos * parentWorldInv;

                // Account for shape offset
                localPos -= worldRot * bone.localOffset;

                // Set the bone's matrix
                osg::Matrix localMatrix;
                localMatrix.makeRotate(worldRot);
                localMatrix.setTrans(localPos);

                bone.node->setMatrix(localMatrix);
            }
        }

        // Mark skeleton as needing update
        mSkeleton->markDirty();
    }

    void Ragdoll::applyImpulse(const osg::Vec3f& impulse, const osg::Vec3f& worldPoint)
    {
        // Find the closest body to the world point
        float closestDist = std::numeric_limits<float>::max();
        JPH::BodyID closestBody;

        for (const JPH::BodyID& bodyId : mBodyIds)
        {
            JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), bodyId);
            if (lock.Succeeded())
            {
                osg::Vec3f bodyPos = Misc::Convert::toOsg(lock.GetBody().GetCenterOfMassPosition());
                float dist = (bodyPos - worldPoint).length2();
                if (dist < closestDist)
                {
                    closestDist = dist;
                    closestBody = bodyId;
                }
            }
        }

        if (!closestBody.IsInvalid())
        {
            JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
            bodyInterface.AddImpulse(closestBody,
                Misc::Convert::toJolt<JPH::Vec3>(impulse),
                Misc::Convert::toJolt<JPH::RVec3>(worldPoint));
        }
    }

    void Ragdoll::applyRootImpulse(const osg::Vec3f& impulse)
    {
        if (!mBodyIds.empty())
        {
            JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
            bodyInterface.AddImpulse(mBodyIds[0], Misc::Convert::toJolt<JPH::Vec3>(impulse));
        }
    }

    bool Ragdoll::isAtRest() const
    {
        for (const JPH::BodyID& bodyId : mBodyIds)
        {
            JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), bodyId);
            if (lock.Succeeded() && lock.GetBody().IsActive())
            {
                return false;
            }
        }
        return true;
    }

    void Ragdoll::activate()
    {
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        for (const JPH::BodyID& bodyId : mBodyIds)
        {
            bodyInterface.ActivateBody(bodyId);
        }
    }

    osg::Vec3f Ragdoll::getPosition() const
    {
        if (mBodyIds.empty())
            return osg::Vec3f();

        JPH::BodyLockRead lock(mTaskScheduler->getBodyLockInterface(), mBodyIds[0]);
        if (lock.Succeeded())
        {
            return Misc::Convert::toOsg(lock.GetBody().GetCenterOfMassPosition());
        }
        return osg::Vec3f();
    }

    JPH::BodyID Ragdoll::getRootBodyId() const
    {
        if (mBodyIds.empty())
            return JPH::BodyID();
        return mBodyIds[0];
    }
}
