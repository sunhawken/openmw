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
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <osg/NodeVisitor>
#include <osg/Node>

#include <algorithm>
#include <cmath>
#include <map>

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
    // Joint limits tuned for realistic human movement
    static const BoneDef sHumanoidBones[] = {
        // Root/Pelvis - no parent, no constraints
        { "bip01 pelvis", nullptr, 0.15f, BoneDef::Box, 0, 0, 0, 0 },

        // Spine chain - limited flexibility
        { "bip01 spine", "bip01 pelvis", 0.10f, BoneDef::Capsule,
          -0.2f, 0.2f, 0.3f, 0.2f },
        { "bip01 spine1", "bip01 spine", 0.10f, BoneDef::Capsule,
          -0.15f, 0.15f, 0.25f, 0.15f },
        { "bip01 spine2", "bip01 spine1", 0.10f, BoneDef::Capsule,
          -0.15f, 0.15f, 0.25f, 0.15f },

        // Neck and head
        { "bip01 neck", "bip01 spine2", 0.03f, BoneDef::Capsule,
          -0.4f, 0.4f, 0.4f, 0.4f },
        { "bip01 head", "bip01 neck", 0.08f, BoneDef::Sphere,
          -0.3f, 0.3f, 0.3f, 0.3f },

        // Left arm
        { "bip01 l clavicle", "bip01 spine2", 0.02f, BoneDef::Capsule,
          -0.1f, 0.1f, 0.2f, 0.2f },
        { "bip01 l upperarm", "bip01 l clavicle", 0.04f, BoneDef::Capsule,
          -1.0f, 1.0f, 1.0f, 1.0f },  // Shoulder has wide range
        { "bip01 l forearm", "bip01 l upperarm", 0.03f, BoneDef::Capsule,
          -0.05f, 2.2f, 0.05f, 0.05f },  // Elbow mainly bends one way
        { "bip01 l hand", "bip01 l forearm", 0.01f, BoneDef::Box,
          -0.4f, 0.4f, 0.6f, 0.2f },

        // Right arm
        { "bip01 r clavicle", "bip01 spine2", 0.02f, BoneDef::Capsule,
          -0.1f, 0.1f, 0.2f, 0.2f },
        { "bip01 r upperarm", "bip01 r clavicle", 0.04f, BoneDef::Capsule,
          -1.0f, 1.0f, 1.0f, 1.0f },
        { "bip01 r forearm", "bip01 r upperarm", 0.03f, BoneDef::Capsule,
          -0.05f, 2.2f, 0.05f, 0.05f },
        { "bip01 r hand", "bip01 r forearm", 0.01f, BoneDef::Box,
          -0.4f, 0.4f, 0.6f, 0.2f },

        // Left leg
        { "bip01 l thigh", "bip01 pelvis", 0.07f, BoneDef::Capsule,
          -0.3f, 0.3f, 0.9f, 0.3f },  // Hip
        { "bip01 l calf", "bip01 l thigh", 0.05f, BoneDef::Capsule,
          -0.05f, 0.05f, 2.2f, 0.05f },  // Knee mainly bends backward
        { "bip01 l foot", "bip01 l calf", 0.02f, BoneDef::Box,
          -0.2f, 0.2f, 0.4f, 0.2f },

        // Right leg
        { "bip01 r thigh", "bip01 pelvis", 0.07f, BoneDef::Capsule,
          -0.3f, 0.3f, 0.9f, 0.3f },
        { "bip01 r calf", "bip01 r thigh", 0.05f, BoneDef::Capsule,
          -0.05f, 0.05f, 2.2f, 0.05f },
        { "bip01 r foot", "bip01 r calf", 0.02f, BoneDef::Box,
          -0.2f, 0.2f, 0.4f, 0.2f },
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

            // Estimate bone dimensions by finding any child bone
            osg::Vec3f boneSize(8.0f * scale, 8.0f * scale, 15.0f * scale);  // Default size

            // Search through all bones to find children of this bone
            for (int j = 0; j < sNumHumanoidBones; ++j)
            {
                if (sHumanoidBones[j].parentName &&
                    std::string(sHumanoidBones[j].parentName) == boneDef.name)
                {
                    osg::MatrixTransform* childBone = findBone(skeleton, sHumanoidBones[j].name);
                    if (childBone)
                    {
                        osg::Vec3f estimatedSize = estimateBoneSize(boneNode, childBone);
                        // Take the largest child distance for better coverage
                        if (estimatedSize.z() > boneSize.z())
                            boneSize = estimatedSize;
                    }
                }
            }

            // Ensure minimum size but keep shapes smaller to avoid overlap
            boneSize.x() = std::max(boneSize.x(), 4.0f * scale);
            boneSize.y() = std::max(boneSize.y(), 4.0f * scale);
            boneSize.z() = std::max(boneSize.z(), 8.0f * scale);

            // Create appropriate shape - use smaller shapes to avoid initial overlap
            JPH::Ref<JPH::Shape> shape;
            osg::Vec3f shapeOffset(0, 0, 0);

            // Scale down shapes to 70% to prevent initial overlaps
            const float shapeScale = 0.7f;

            switch (boneDef.shapeType)
            {
                case BoneDef::Sphere:
                {
                    float radius = std::max({boneSize.x(), boneSize.y(), boneSize.z()}) * 0.4f * shapeScale;
                    shape = new JPH::SphereShape(radius);
                    break;
                }
                case BoneDef::Box:
                {
                    float halfX = boneSize.x() * 0.4f * shapeScale;
                    float halfY = boneSize.y() * 0.4f * shapeScale;
                    float halfZ = boneSize.z() * 0.4f * shapeScale;
                    shape = new JPH::BoxShape(JPH::Vec3(halfX, halfY, halfZ));
                    shapeOffset = osg::Vec3f(0, 0, boneSize.z() * 0.5f);
                    break;
                }
                case BoneDef::Capsule:
                default:
                {
                    // Capsule along bone direction (Z axis)
                    float radius = std::min(boneSize.x(), boneSize.y()) * 0.3f * shapeScale;
                    float halfHeight = (boneSize.z() * 0.5f - radius) * shapeScale;
                    if (halfHeight < 0.0f)
                    {
                        halfHeight = 0.0f;
                        radius = boneSize.z() * 0.4f * shapeScale;
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
            // Higher damping for stable ragdoll simulation
            settings.mLinearDamping = 0.5f;
            settings.mAngularDamping = 0.8f;
            settings.mFriction = 0.8f;
            settings.mRestitution = 0.0f;  // No bouncing for corpses
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

        // Create all bodies but DO NOT activate them yet
        // We need to add constraints first to prevent explosion
        for (auto& settings : bodySettings)
        {
            JPH::Body* body = mTaskScheduler->createPhysicsBody(settings);
            if (body)
            {
                body->SetUserData(0);  // Don't associate with PtrHolder
                mTaskScheduler->addCollisionObject(body, false);  // false = don't activate yet
                mBodyIds.push_back(body->GetID());
            }
        }

        // Create constraints between parent-child bone pairs BEFORE activating bodies
        JPH::PhysicsSystem* joltSystem = mPhysicsSystem->getJoltSystem();
        if (joltSystem && mBodyIds.size() > 1)
        {
            // Map bone names to body indices
            std::map<std::string, int> boneToBodyIndex;
            for (size_t i = 0; i < mBones.size(); ++i)
            {
                boneToBodyIndex[mBones[i].name] = static_cast<int>(i);
            }

            // Create constraints for each bone with a parent
            for (int i = 0; i < sNumHumanoidBones; ++i)
            {
                const BoneDef& boneDef = sHumanoidBones[i];
                if (!boneDef.parentName)
                    continue;  // Root bone has no parent

                // Find body indices for child and parent
                auto childIt = boneToBodyIndex.find(boneDef.name);
                auto parentIt = boneToBodyIndex.find(boneDef.parentName);

                if (childIt == boneToBodyIndex.end() || parentIt == boneToBodyIndex.end())
                    continue;  // One of the bones wasn't created

                int childIdx = childIt->second;
                int parentIdx = parentIt->second;

                if (childIdx >= static_cast<int>(mBodyIds.size()) || parentIdx >= static_cast<int>(mBodyIds.size()))
                    continue;

                JPH::BodyID childBodyId = mBodyIds[childIdx];
                JPH::BodyID parentBodyId = mBodyIds[parentIdx];

                // Get bone nodes for calculating proper joint axes
                osg::MatrixTransform* childNode = mBones[childIdx].node;
                osg::MatrixTransform* parentNode = mBones[parentIdx].node;

                // Calculate constraint position (at the child bone's origin, which is the joint)
                osg::Matrix childWorld = getWorldMatrix(childNode);
                osg::Matrix parentWorld = getWorldMatrix(parentNode);
                osg::Vec3f childWorldPos = childWorld.getTrans();
                osg::Vec3f parentWorldPos = parentWorld.getTrans();

                // Calculate the bone direction from parent to child - this is the twist axis
                osg::Vec3f boneDirection = childWorldPos - parentWorldPos;
                float boneLength = boneDirection.length();
                if (boneLength > 0.001f)
                    boneDirection /= boneLength;
                else
                    boneDirection = osg::Vec3f(0, 0, 1);  // Default to Z if bones are at same position

                // Create an orthonormal basis for the constraint
                osg::Vec3f twistAxis = boneDirection;
                osg::Vec3f planeAxis;

                // Find a perpendicular axis
                if (std::abs(twistAxis.z()) < 0.9f)
                    planeAxis = twistAxis ^ osg::Vec3f(0, 0, 1);
                else
                    planeAxis = twistAxis ^ osg::Vec3f(1, 0, 0);
                planeAxis.normalize();

                // Get body positions for constraint setup
                JPH::BodyLockRead childLock(mTaskScheduler->getBodyLockInterface(), childBodyId);
                JPH::BodyLockRead parentLock(mTaskScheduler->getBodyLockInterface(), parentBodyId);

                if (!childLock.Succeeded() || !parentLock.Succeeded())
                    continue;

                const JPH::Body& childBody = childLock.GetBody();
                const JPH::Body& parentBody = parentLock.GetBody();

                JPH::RVec3 constraintWorldPos = Misc::Convert::toJolt<JPH::RVec3>(childWorldPos);

                // Create SwingTwist constraint for realistic joint limits
                JPH::SwingTwistConstraintSettings constraintSettings;

                // Set up constraint in body local space
                // Position relative to each body's center of mass
                constraintSettings.mPosition1 = parentBody.GetInverseCenterOfMassTransform() * constraintWorldPos;
                constraintSettings.mPosition2 = childBody.GetInverseCenterOfMassTransform() * constraintWorldPos;

                // Use the calculated bone direction for twist axis
                JPH::Vec3 joltTwistAxis = Misc::Convert::toJolt<JPH::Vec3>(twistAxis);
                JPH::Vec3 joltPlaneAxis = Misc::Convert::toJolt<JPH::Vec3>(planeAxis);

                constraintSettings.mTwistAxis1 = joltTwistAxis;
                constraintSettings.mTwistAxis2 = joltTwistAxis;
                constraintSettings.mPlaneAxis1 = joltPlaneAxis;
                constraintSettings.mPlaneAxis2 = joltPlaneAxis;

                // Apply joint limits from bone definition
                constraintSettings.mNormalHalfConeAngle = boneDef.swingY;
                constraintSettings.mPlaneHalfConeAngle = boneDef.swingZ;
                constraintSettings.mTwistMinAngle = boneDef.twistMin;
                constraintSettings.mTwistMaxAngle = boneDef.twistMax;

                // Create and add the constraint
                JPH::Ref<JPH::Constraint> constraint = constraintSettings.Create(
                    const_cast<JPH::Body&>(parentBody),
                    const_cast<JPH::Body&>(childBody));

                joltSystem->AddConstraint(constraint);
                mConstraints.push_back(constraint);
            }

            Log(Debug::Info) << "Ragdoll: Created " << mConstraints.size() << " constraints";
        }

        // NOW activate all bodies after constraints are in place
        JPH::BodyInterface& bodyInterface = mTaskScheduler->getBodyInterface();
        for (const JPH::BodyID& bodyId : mBodyIds)
        {
            bodyInterface.ActivateBody(bodyId);
        }

        Log(Debug::Info) << "Ragdoll: Created ragdoll with " << mBodyIds.size()
                         << " bodies for " << ptr.getCellRef().getRefId();
    }

    Ragdoll::~Ragdoll()
    {
        // Remove all constraints first
        JPH::PhysicsSystem* joltSystem = mPhysicsSystem->getJoltSystem();
        if (joltSystem)
        {
            for (auto& constraint : mConstraints)
            {
                joltSystem->RemoveConstraint(constraint);
            }
        }
        mConstraints.clear();

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

        // First pass: collect all body transforms
        // We need to do this before modifying any bones to avoid hierarchy issues
        struct BoneTransform {
            osg::Vec3f worldPos;
            osg::Quat worldRot;
            bool valid;
        };
        std::vector<BoneTransform> transforms(mBones.size());

        for (size_t i = 0; i < mBones.size(); ++i)
        {
            const RagdollBone& bone = mBones[i];
            transforms[i].valid = false;

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

            // Convert to OSG - this is the position of the shape center
            osg::Vec3f shapeWorldPos = Misc::Convert::toOsg(bodyPos);
            osg::Quat worldRot = Misc::Convert::toOsg(bodyRot);

            // The body was placed at: bonePos + boneRot * offset
            // So the bone position is: bodyPos - bodyRot * offset
            // (We use bodyRot since the body rotates with the bone)
            osg::Vec3f boneWorldPos = shapeWorldPos - worldRot * bone.localOffset;

            transforms[i].worldPos = boneWorldPos;
            transforms[i].worldRot = worldRot;
            transforms[i].valid = true;
        }

        // Second pass: apply transforms
        // Process from root to leaves to ensure parent transforms are set first
        for (size_t i = 0; i < mBones.size(); ++i)
        {
            const RagdollBone& bone = mBones[i];
            if (!transforms[i].valid || !bone.node)
                continue;

            osg::Vec3f boneWorldPos = transforms[i].worldPos;
            osg::Quat worldRot = transforms[i].worldRot;

            // Calculate local transform relative to parent
            osg::Node* parent = bone.node->getParent(0);
            if (parent)
            {
                // Get the parent's world matrix (this should be stable since we process root-to-leaf)
                osg::Matrix parentWorldInv = osg::Matrix::inverse(getWorldMatrix(parent));

                // Transform bone world position to parent's local space
                osg::Vec3f localPos = boneWorldPos * parentWorldInv;

                // Calculate the local rotation by removing parent's rotation
                osg::Quat parentWorldRot = getWorldMatrix(parent).getRotate();
                osg::Quat localRot = parentWorldRot.inverse() * worldRot;

                // Set the bone's local matrix
                osg::Matrix localMatrix;
                localMatrix.makeRotate(localRot);
                localMatrix.setTrans(localPos);

                bone.node->setMatrix(localMatrix);
            }
        }

        // Note: We do NOT call mSkeleton->markDirty() here!
        // markDirty() clears the bone cache and forces full re-initialization,
        // which is wrong - we've already updated the bone transforms via setMatrix().
        // The skeleton will pick up our changes on the next traversal.
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
