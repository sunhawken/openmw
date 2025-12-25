#include "ragdollbuilder.hpp"

#include "joltlayers.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>

#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>

namespace MWPhysics
{
    namespace
    {
        // Physics skeleton hierarchy - defines the correct anatomical parent for each bone.
        // Key = bone name (lowercase), Value = parent bone name (lowercase), empty for root.
        const std::unordered_map<std::string, std::string> sPhysicsHierarchy = {
            // Root
            {"bip01 pelvis", ""},

            // Spine chain
            {"bip01 spine", "bip01 pelvis"},
            {"bip01 spine1", "bip01 spine"},
            {"bip01 spine2", "bip01 spine1"},

            // Head chain
            {"bip01 neck", "bip01 spine2"},
            {"bip01 head", "bip01 neck"},

            // Left arm
            {"bip01 l clavicle", "bip01 spine2"},
            {"bip01 l upperarm", "bip01 l clavicle"},
            {"bip01 l forearm", "bip01 l upperarm"},
            {"bip01 l hand", "bip01 l forearm"},

            // Right arm
            {"bip01 r clavicle", "bip01 spine2"},
            {"bip01 r upperarm", "bip01 r clavicle"},
            {"bip01 r forearm", "bip01 r upperarm"},
            {"bip01 r hand", "bip01 r forearm"},

            // Left leg
            {"bip01 l thigh", "bip01 pelvis"},
            {"bip01 l calf", "bip01 l thigh"},
            {"bip01 l foot", "bip01 l calf"},

            // Right leg
            {"bip01 r thigh", "bip01 pelvis"},
            {"bip01 r calf", "bip01 r thigh"},
            {"bip01 r foot", "bip01 r calf"},
        };

        // Processing order ensures parents are processed before children
        const std::vector<std::string> sProcessingOrder = {
            "bip01 pelvis",
            "bip01 spine", "bip01 spine1", "bip01 spine2",
            "bip01 neck", "bip01 head",
            "bip01 l clavicle", "bip01 l upperarm", "bip01 l forearm", "bip01 l hand",
            "bip01 r clavicle", "bip01 r upperarm", "bip01 r forearm", "bip01 r hand",
            "bip01 l thigh", "bip01 l calf", "bip01 l foot",
            "bip01 r thigh", "bip01 r calf", "bip01 r foot",
        };

        // Visitor to collect bone nodes
        class CollectBonesVisitor : public osg::NodeVisitor
        {
        public:
            CollectBonesVisitor() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}

            void apply(osg::MatrixTransform& node) override
            {
                if (!node.getName().empty())
                    mBones[Misc::StringUtils::lowerCase(node.getName())] = &node;
                traverse(node);
            }

            std::unordered_map<std::string, osg::MatrixTransform*> mBones;
        };

        bool nameContains(const std::string& name, const std::string& pattern)
        {
            return name.find(pattern) != std::string::npos;
        }
    }

    bool RagdollSettingsBuilder::isPhysicsBone(const std::string& lowerName)
    {
        return sPhysicsHierarchy.find(lowerName) != sPhysicsHierarchy.end();
    }

    std::string RagdollSettingsBuilder::getPhysicsParent(const std::string& lowerName)
    {
        auto it = sPhysicsHierarchy.find(lowerName);
        return (it != sPhysicsHierarchy.end()) ? it->second : "";
    }

    const std::vector<std::string>& RagdollSettingsBuilder::getPhysicsBoneNames()
    {
        return sProcessingOrder;
    }

    osg::Matrix RagdollSettingsBuilder::getWorldMatrix(osg::Node* node)
    {
        osg::Matrix worldMatrix;
        if (!node)
            return worldMatrix;

        osg::NodePathList nodePaths = node->getParentalNodePaths();
        if (!nodePaths.empty())
            worldMatrix = osg::computeLocalToWorld(nodePaths[0]);

        return worldMatrix;
    }

    JointConfig RagdollSettingsBuilder::getDefaultConfig(const std::string& boneName)
    {
        std::string name = Misc::StringUtils::lowerCase(boneName);
        JointConfig config;

        if (nameContains(name, "pelvis") || nameContains(name, "root"))
        {
            config.mass = 0.15f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.0f;
            config.twistMinLimit = 0.0f;
            config.twistMaxLimit = 0.0f;
        }
        else if (nameContains(name, "spine"))
        {
            config.mass = 0.10f;
            config.swingLimit = 0.25f;
            config.twistMinLimit = -0.15f;
            config.twistMaxLimit = 0.15f;
        }
        else if (nameContains(name, "neck"))
        {
            config.mass = 0.03f;
            config.swingLimit = 0.4f;
            config.twistMinLimit = -0.4f;
            config.twistMaxLimit = 0.4f;
        }
        else if (nameContains(name, "head"))
        {
            config.mass = 0.08f;
            config.shapeType = JointConfig::ShapeType::Sphere;
            config.swingLimit = 0.3f;
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }
        else if (nameContains(name, "clavicle"))
        {
            config.mass = 0.02f;
            config.swingLimit = 0.2f;
            config.twistMinLimit = -0.1f;
            config.twistMaxLimit = 0.1f;
        }
        else if (nameContains(name, "upperarm"))
        {
            config.mass = 0.04f;
            config.swingLimit = 0.8f;
            config.twistMinLimit = -0.6f;
            config.twistMaxLimit = 0.6f;
        }
        else if (nameContains(name, "forearm"))
        {
            config.mass = 0.03f;
            config.swingLimit = 0.15f;
            config.twistMinLimit = 0.0f;
            config.twistMaxLimit = 2.0f;
        }
        else if (nameContains(name, "hand"))
        {
            config.mass = 0.01f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.5f;
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }
        else if (nameContains(name, "thigh"))
        {
            config.mass = 0.07f;
            config.swingLimit = 0.7f;
            config.twistMinLimit = -0.25f;
            config.twistMaxLimit = 0.25f;
        }
        else if (nameContains(name, "calf"))
        {
            config.mass = 0.05f;
            config.swingLimit = 0.15f;
            config.twistMinLimit = 0.0f;
            config.twistMaxLimit = 2.0f;
        }
        else if (nameContains(name, "foot"))
        {
            config.mass = 0.02f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.35f;
            config.twistMinLimit = -0.15f;
            config.twistMaxLimit = 0.15f;
        }
        else
        {
            config.mass = 0.02f;
            config.swingLimit = 0.5f;
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }

        return config;
    }

    JPH::Ref<JPH::Shape> RagdollSettingsBuilder::createBoneShape(
        const BoneData& bone,
        const JointConfig& config,
        float scale)
    {
        const float shapeScale = config.shapeScale;
        float length = bone.boneLength;
        float width = length * 0.3f;

        // Ensure minimum dimensions
        length = std::max(length, 8.0f * scale);
        width = std::max(width, 4.0f * scale);

        // Calculate local bone direction from world rotation
        // The bone points along its local Z axis in the bind pose
        osg::Vec3f localBoneDir = bone.worldRotation.inverse() * bone.boneDirection;
        if (localBoneDir.length2() < 0.001f)
            localBoneDir = osg::Vec3f(0, 0, 1);
        else
            localBoneDir.normalize();

        // Shape center is at half the bone length along the bone direction
        osg::Vec3f shapeCenter = localBoneDir * (length * 0.5f);

        JPH::Ref<JPH::Shape> shape;

        switch (config.shapeType)
        {
            case JointConfig::ShapeType::Sphere:
            {
                float radius = length * 0.4f * shapeScale;
                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    JPH::Quat::sIdentity(),
                    new JPH::SphereShape(radius));
                break;
            }
            case JointConfig::ShapeType::Box:
            {
                float halfX = width * 0.4f * shapeScale;
                float halfY = width * 0.4f * shapeScale;
                float halfZ = length * 0.4f * shapeScale;

                osg::Quat boxRot;
                boxRot.makeRotate(osg::Vec3f(0, 0, 1), localBoneDir);

                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    Misc::Convert::toJolt(boxRot),
                    new JPH::BoxShape(JPH::Vec3(halfX, halfY, halfZ)));
                break;
            }
            case JointConfig::ShapeType::Capsule:
            default:
            {
                float radius = width * 0.3f * shapeScale;
                float halfHeight = (length * 0.5f - radius) * shapeScale;
                if (halfHeight < 0.0f)
                {
                    halfHeight = 0.0f;
                    radius = length * 0.4f * shapeScale;
                }

                // Jolt capsules are along Y axis, rotate to align with bone
                osg::Quat capsuleRot;
                capsuleRot.makeRotate(osg::Vec3f(0, 1, 0), localBoneDir);

                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    Misc::Convert::toJolt(capsuleRot),
                    new JPH::CapsuleShape(halfHeight, radius));
                break;
            }
        }

        // Move center of mass to body origin (joint position) for proper constraint behavior
        JPH::Vec3 comOffset = shape->GetCenterOfMass();
        if (comOffset.LengthSq() > 0.0001f)
            shape = new JPH::OffsetCenterOfMassShape(shape, -comOffset);

        return shape;
    }

    JPH::Ref<JPH::TwoBodyConstraintSettings> RagdollSettingsBuilder::createConstraintSettings(
        const BoneData& parentBone,
        const BoneData& childBone,
        const JointConfig& config)
    {
        auto settings = new JPH::SwingTwistConstraintSettings;
        settings->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

        // The constraint point is at the child bone's origin (where it connects to parent)
        osg::Vec3f jointWorldPos = childBone.worldPosition;

        // Parent's local constraint position
        osg::Vec3f parentToJoint = jointWorldPos - parentBone.worldPosition;
        osg::Vec3f parentLocalPos = parentBone.worldRotation.inverse() * parentToJoint;

        // Child's local constraint position is at its origin
        osg::Vec3f childLocalPos(0, 0, 0);

        settings->mPosition1 = JPH::RVec3(parentLocalPos.x(), parentLocalPos.y(), parentLocalPos.z());
        settings->mPosition2 = JPH::RVec3(childLocalPos.x(), childLocalPos.y(), childLocalPos.z());

        // Twist axis is the bone direction (from parent to child)
        osg::Vec3f boneDir = childBone.worldPosition - parentBone.worldPosition;
        float boneLength = boneDir.length();
        if (boneLength > 0.001f)
            boneDir /= boneLength;
        else
            boneDir = osg::Vec3f(0, 0, 1);

        // Transform to local space for each body
        osg::Vec3f parentLocalTwist = parentBone.worldRotation.inverse() * boneDir;
        parentLocalTwist.normalize();

        osg::Vec3f childLocalTwist = childBone.worldRotation.inverse() * boneDir;
        childLocalTwist.normalize();

        settings->mTwistAxis1 = Misc::Convert::toJolt<JPH::Vec3>(parentLocalTwist);
        settings->mTwistAxis2 = Misc::Convert::toJolt<JPH::Vec3>(childLocalTwist);

        // Plane axis perpendicular to twist
        osg::Vec3f worldUp(0, 0, 1);
        if (std::abs(boneDir * worldUp) > 0.9f)
            worldUp = osg::Vec3f(1, 0, 0);

        osg::Vec3f planeDir = boneDir ^ worldUp;
        planeDir.normalize();

        // Ensure perpendicularity
        float dotCheck = boneDir * planeDir;
        if (std::abs(dotCheck) > 0.01f)
        {
            planeDir = planeDir - boneDir * dotCheck;
            planeDir.normalize();
        }

        osg::Vec3f parentLocalPlane = parentBone.worldRotation.inverse() * planeDir;
        parentLocalPlane.normalize();

        osg::Vec3f childLocalPlane = childBone.worldRotation.inverse() * planeDir;
        childLocalPlane.normalize();

        settings->mPlaneAxis1 = Misc::Convert::toJolt<JPH::Vec3>(parentLocalPlane);
        settings->mPlaneAxis2 = Misc::Convert::toJolt<JPH::Vec3>(childLocalPlane);

        // Joint limits
        settings->mNormalHalfConeAngle = config.swingLimit;
        settings->mPlaneHalfConeAngle = config.swingLimit;
        settings->mTwistMinAngle = config.twistMinLimit;
        settings->mTwistMaxAngle = config.twistMaxLimit;

        return settings;
    }

    JPH::Ref<JPH::RagdollSettings> RagdollSettingsBuilder::build(
        SceneUtil::Skeleton* osgSkeleton,
        float totalMass,
        float scale,
        const JointConfigMap* overrides)
    {
        if (!osgSkeleton)
        {
            Log(Debug::Error) << "RagdollSettingsBuilder: null skeleton";
            return nullptr;
        }

        // Collect OSG bones
        CollectBonesVisitor collector;
        osgSkeleton->accept(collector);

        Log(Debug::Verbose) << "RagdollSettingsBuilder: Found " << collector.mBones.size() << " bones";

        // Build bone data for physics bones
        std::unordered_map<std::string, BoneData> boneDataMap;
        std::unordered_map<std::string, int> boneToJoltIndex;

        JPH::Ref<JPH::Skeleton> joltSkeleton = new JPH::Skeleton;
        JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
        float massSum = 0.0f;

        for (const std::string& boneName : sProcessingOrder)
        {
            auto nodeIt = collector.mBones.find(boneName);
            if (nodeIt == collector.mBones.end())
            {
                Log(Debug::Verbose) << "RagdollSettingsBuilder: Bone not found: " << boneName;
                continue;
            }

            osg::MatrixTransform* boneNode = nodeIt->second;
            std::string parentName = getPhysicsParent(boneName);

            // Get Jolt parent index
            int parentJoltIndex = -1;
            if (!parentName.empty())
            {
                auto parentIt = boneToJoltIndex.find(parentName);
                if (parentIt == boneToJoltIndex.end())
                {
                    Log(Debug::Warning) << "RagdollSettingsBuilder: Parent not processed: " << parentName;
                    continue;
                }
                parentJoltIndex = parentIt->second;
            }

            // Create Jolt joint
            int joltIndex = (parentJoltIndex >= 0)
                ? joltSkeleton->AddJoint(boneName.c_str(), parentJoltIndex)
                : joltSkeleton->AddJoint(boneName.c_str());
            boneToJoltIndex[boneName] = joltIndex;

            // Collect bone data
            BoneData bone;
            bone.osgNode = boneNode;
            bone.name = boneName;
            bone.physicsParentName = parentName;
            bone.joltJointIndex = joltIndex;

            osg::Matrix worldMatrix = getWorldMatrix(boneNode);
            bone.worldPosition = worldMatrix.getTrans();
            bone.worldRotation = worldMatrix.getRotate();

            // Find bone direction and length by looking for physics child
            bone.boneDirection = osg::Vec3f(0, 0, 1);
            bone.boneLength = 15.0f * scale;

            for (const auto& [childName, childParent] : sPhysicsHierarchy)
            {
                if (childParent == boneName)
                {
                    auto childIt = collector.mBones.find(childName);
                    if (childIt != collector.mBones.end())
                    {
                        osg::Matrix childWorld = getWorldMatrix(childIt->second);
                        osg::Vec3f childPos = childWorld.getTrans();
                        bone.boneDirection = childPos - bone.worldPosition;
                        bone.boneLength = bone.boneDirection.length();
                        if (bone.boneLength > 0.001f)
                            bone.boneDirection /= bone.boneLength;
                        else
                            bone.boneDirection = osg::Vec3f(0, 0, 1);
                    }
                    break;
                }
            }

            // For leaf bones, use parent direction
            if (bone.boneLength < 1.0f && !parentName.empty())
            {
                auto parentIt = boneDataMap.find(parentName);
                if (parentIt != boneDataMap.end())
                {
                    bone.boneDirection = parentIt->second.boneDirection;
                    bone.boneLength = parentIt->second.boneLength * 0.7f;
                }
            }

            boneDataMap[boneName] = bone;

            // Get config
            JointConfig config = getDefaultConfig(boneName);
            if (overrides)
            {
                auto it = overrides->find(boneName);
                if (it != overrides->end())
                    config = it->second;
            }

            // Create shape
            JPH::Ref<JPH::Shape> shape = createBoneShape(bone, config, scale);

            // Create part
            JPH::RagdollSettings::Part& part = settings->mParts.emplace_back();
            part.SetShape(shape);
            part.mPosition = Misc::Convert::toJolt<JPH::RVec3>(bone.worldPosition);
            part.mRotation = Misc::Convert::toJolt(bone.worldRotation);
            part.mMotionType = JPH::EMotionType::Dynamic;
            part.mObjectLayer = Layers::DEBRIS;

            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = config.mass;
            massSum += config.mass;

            part.mLinearDamping = 0.5f;
            part.mAngularDamping = 0.8f;
            part.mFriction = 0.8f;
            part.mRestitution = 0.0f;
            part.mMotionQuality = JPH::EMotionQuality::LinearCast;
            part.mAllowSleeping = true;
            part.mGravityFactor = 1.0f;

            // Create constraint to parent
            if (parentJoltIndex >= 0 && !parentName.empty())
            {
                auto parentIt = boneDataMap.find(parentName);
                if (parentIt != boneDataMap.end())
                {
                    part.mToParent = createConstraintSettings(parentIt->second, bone, config);
                }
            }
        }

        if (settings->mParts.empty())
        {
            Log(Debug::Warning) << "RagdollSettingsBuilder: No parts created";
            return nullptr;
        }

        // Normalize masses
        if (massSum > 0.0f)
        {
            float massScale = totalMass / massSum;
            for (auto& part : settings->mParts)
                part.mMassPropertiesOverride.mMass *= massScale;
        }

        settings->mSkeleton = joltSkeleton;
        settings->CalculateBodyIndexToConstraintIndex();
        settings->CalculateConstraintIndexToBodyIdxPair();

        if (!settings->Stabilize())
            Log(Debug::Warning) << "RagdollSettingsBuilder: Failed to stabilize constraints";

        Log(Debug::Info) << "RagdollSettingsBuilder: Created ragdoll with "
                         << settings->mParts.size() << " parts";

        return settings;
    }
}
