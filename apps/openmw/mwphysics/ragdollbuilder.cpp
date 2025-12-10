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
#include <set>

namespace MWPhysics
{
    namespace
    {
        // Physics skeleton hierarchy - defines the CORRECT anatomical parent for each bone
        // This overrides the OSG scene graph hierarchy which may be different
        // Key = bone name (lowercase), Value = parent bone name (lowercase), empty for root
        const std::unordered_map<std::string, std::string> sPhysicsHierarchy = {
            // Root
            {"bip01 pelvis", ""},  // Root bone, no parent

            // Spine chain
            {"bip01 spine", "bip01 pelvis"},
            {"bip01 spine1", "bip01 spine"},
            {"bip01 spine2", "bip01 spine1"},

            // Head chain (from spine2)
            {"bip01 neck", "bip01 spine2"},
            {"bip01 head", "bip01 neck"},

            // Left arm (from spine2, NOT from neck!)
            {"bip01 l clavicle", "bip01 spine2"},
            {"bip01 l upperarm", "bip01 l clavicle"},
            {"bip01 l forearm", "bip01 l upperarm"},
            {"bip01 l hand", "bip01 l forearm"},

            // Right arm (from spine2, NOT from neck!)
            {"bip01 r clavicle", "bip01 spine2"},
            {"bip01 r upperarm", "bip01 r clavicle"},
            {"bip01 r forearm", "bip01 r upperarm"},
            {"bip01 r hand", "bip01 r forearm"},

            // Left leg (from pelvis, NOT from spine!)
            {"bip01 l thigh", "bip01 pelvis"},
            {"bip01 l calf", "bip01 l thigh"},
            {"bip01 l foot", "bip01 l calf"},

            // Right leg (from pelvis, NOT from spine!)
            {"bip01 r thigh", "bip01 pelvis"},
            {"bip01 r calf", "bip01 r thigh"},
            {"bip01 r foot", "bip01 r calf"},
        };

        // Check if a bone should be included in the ragdoll physics
        bool isPhysicsBone(const std::string& lowerName)
        {
            return sPhysicsHierarchy.find(lowerName) != sPhysicsHierarchy.end();
        }

        // Get the physics parent for a bone (may differ from OSG hierarchy)
        std::string getPhysicsParent(const std::string& lowerName)
        {
            auto it = sPhysicsHierarchy.find(lowerName);
            if (it != sPhysicsHierarchy.end())
                return it->second;
            return "";
        }

        // Visitor to find all bone nodes in a skeleton
        class CollectBonesVisitor : public osg::NodeVisitor
        {
        public:
            CollectBonesVisitor()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::MatrixTransform& node) override
            {
                if (!node.getName().empty())
                {
                    mBones.push_back(&node);
                }
                traverse(node);
            }

            std::vector<osg::MatrixTransform*> mBones;
        };

        // Check if bone name matches common patterns
        bool nameContains(const std::string& name, const std::string& pattern)
        {
            return name.find(pattern) != std::string::npos;
        }

        // Calculate a quaternion that rotates local Z to point along the given direction
        // This creates a rotation where:
        // - Local Z points along 'direction'
        // - Local X and Y are perpendicular, using worldUp as a hint
        osg::Quat makeRotationAlongZ(const osg::Vec3f& direction, const osg::Vec3f& worldUp = osg::Vec3f(0, 0, 1))
        {
            osg::Vec3f dir = direction;
            if (dir.length2() < 0.0001f)
                return osg::Quat();  // Identity if no direction

            dir.normalize();

            // Handle case where direction is parallel to worldUp
            osg::Vec3f up = worldUp;
            if (std::abs(dir * up) > 0.99f)
                up = osg::Vec3f(1, 0, 0);  // Use X as up if direction is vertical

            // Build orthonormal basis
            osg::Vec3f right = dir ^ up;
            right.normalize();
            osg::Vec3f actualUp = right ^ dir;
            actualUp.normalize();

            // Create rotation matrix from basis vectors
            // Column 0 = X (right), Column 1 = Y (up), Column 2 = Z (dir)
            osg::Matrix rotMatrix;
            rotMatrix.set(
                right.x(), right.y(), right.z(), 0,
                actualUp.x(), actualUp.y(), actualUp.z(), 0,
                dir.x(), dir.y(), dir.z(), 0,
                0, 0, 0, 1
            );

            return rotMatrix.getRotate();
        }
    }

    bool RagdollSettingsBuilder::isBone(osg::Node* node)
    {
        auto* mt = dynamic_cast<osg::MatrixTransform*>(node);
        return mt && !mt->getName().empty();
    }

    osg::Matrix RagdollSettingsBuilder::getWorldMatrix(osg::Node* node)
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

    osg::MatrixTransform* RagdollSettingsBuilder::findFirstChildBone(osg::MatrixTransform* node)
    {
        for (unsigned int i = 0; i < node->getNumChildren(); ++i)
        {
            auto* child = dynamic_cast<osg::MatrixTransform*>(node->getChild(i));
            if (child && !child->getName().empty())
                return child;

            // Recursively search in case there are intermediate nodes
            if (node->getChild(i)->asGroup())
            {
                for (unsigned int j = 0; j < node->getChild(i)->asGroup()->getNumChildren(); ++j)
                {
                    auto* grandchild = dynamic_cast<osg::MatrixTransform*>(
                        node->getChild(i)->asGroup()->getChild(j));
                    if (grandchild && !grandchild->getName().empty())
                        return grandchild;
                }
            }
        }
        return nullptr;
    }

    JointConfig RagdollSettingsBuilder::getDefaultConfig(const std::string& boneName)
    {
        std::string name = Misc::StringUtils::lowerCase(boneName);
        JointConfig config;

        // Root/Pelvis - heavy, box shape
        if (nameContains(name, "pelvis") || nameContains(name, "root"))
        {
            config.mass = 0.15f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.0f;  // Root has no constraints
            config.twistMinLimit = 0.0f;
            config.twistMaxLimit = 0.0f;
        }
        // Spine - limited movement
        else if (nameContains(name, "spine"))
        {
            config.mass = 0.10f;
            config.swingLimit = 0.25f;
            config.twistMinLimit = -0.15f;
            config.twistMaxLimit = 0.15f;
        }
        // Neck
        else if (nameContains(name, "neck"))
        {
            config.mass = 0.03f;
            config.swingLimit = 0.4f;
            config.twistMinLimit = -0.4f;
            config.twistMaxLimit = 0.4f;
        }
        // Head - sphere shape
        else if (nameContains(name, "head"))
        {
            config.mass = 0.08f;
            config.shapeType = JointConfig::ShapeType::Sphere;
            config.swingLimit = 0.3f;
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }
        // Clavicle/Shoulder
        else if (nameContains(name, "clavicle"))
        {
            config.mass = 0.02f;
            config.swingLimit = 0.2f;
            config.twistMinLimit = -0.1f;
            config.twistMaxLimit = 0.1f;
        }
        // Upper arm
        else if (nameContains(name, "upperarm") || nameContains(name, "upper arm"))
        {
            config.mass = 0.04f;
            config.swingLimit = 0.8f;  // Shoulder - reduced from 1.2 for stability (~46 degrees)
            config.twistMinLimit = -0.6f;
            config.twistMaxLimit = 0.6f;
        }
        // Forearm/Lower arm
        else if (nameContains(name, "forearm") || nameContains(name, "lower arm"))
        {
            config.mass = 0.03f;
            config.swingLimit = 0.15f;  // Elbow - small swing for stability
            config.twistMinLimit = 0.0f;  // Elbow can't bend backward
            config.twistMaxLimit = 2.0f;  // Elbow bends forward (~115 degrees)
        }
        // Hand - box shape
        else if (nameContains(name, "hand"))
        {
            config.mass = 0.01f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.5f;  // Wrist flexibility
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }
        // Thigh/Upper leg
        else if (nameContains(name, "thigh") || nameContains(name, "upper leg"))
        {
            config.mass = 0.07f;
            config.swingLimit = 0.7f;  // Hip - reduced from 0.9 for stability (~40 degrees)
            config.twistMinLimit = -0.25f;
            config.twistMaxLimit = 0.25f;
        }
        // Calf/Lower leg
        else if (nameContains(name, "calf") || nameContains(name, "lower leg") || nameContains(name, "shin"))
        {
            config.mass = 0.05f;
            config.swingLimit = 0.15f;  // Knee - small swing for stability
            config.twistMinLimit = 0.0f;  // Knee can't hyperextend backward
            config.twistMaxLimit = 2.0f;  // Knee bends forward (~115 degrees)
        }
        // Foot - box shape
        else if (nameContains(name, "foot"))
        {
            config.mass = 0.02f;
            config.shapeType = JointConfig::ShapeType::Box;
            config.swingLimit = 0.35f;  // Ankle flexibility
            config.twistMinLimit = -0.15f;
            config.twistMaxLimit = 0.15f;
        }
        // Fingers - skip or very light
        else if (nameContains(name, "finger") || nameContains(name, "toe"))
        {
            config.mass = 0.001f;  // Very light, might skip these
            config.swingLimit = 0.3f;
            config.twistMinLimit = -0.1f;
            config.twistMaxLimit = 0.1f;
        }
        // Default for unknown bones
        else
        {
            config.mass = 0.02f;
            config.swingLimit = 0.5f;
            config.twistMinLimit = -0.3f;
            config.twistMaxLimit = 0.3f;
        }

        return config;
    }

    osg::Vec3f RagdollSettingsBuilder::estimateBoneSize(osg::MatrixTransform* bone, float scale)
    {
        // Default size if we can't estimate
        osg::Vec3f defaultSize(8.0f * scale, 8.0f * scale, 15.0f * scale);

        // Try to find a child bone to estimate length
        osg::MatrixTransform* childBone = findFirstChildBone(bone);
        if (!childBone)
            return defaultSize;

        osg::Matrix boneWorld = getWorldMatrix(bone);
        osg::Matrix childWorld = getWorldMatrix(childBone);

        osg::Vec3f bonePos = boneWorld.getTrans();
        osg::Vec3f childPos = childWorld.getTrans();

        float length = (childPos - bonePos).length();
        if (length < 1.0f)
            return defaultSize;

        // Estimate width as proportion of length
        float width = length * 0.3f;

        return osg::Vec3f(width, width, length);
    }

    JPH::Ref<JPH::Shape> RagdollSettingsBuilder::createBoneShape(
        osg::MatrixTransform* bone,
        const osg::Vec3f& boneSize,
        const JointConfig& config,
        float scale,
        const osg::Vec3f& localBoneDir,
        osg::Vec3f& outShapeOffset)
    {
        const float shapeScale = config.shapeScale;

        // Ensure minimum size
        osg::Vec3f size = boneSize;
        size.x() = std::max(size.x(), 4.0f * scale);
        size.y() = std::max(size.y(), 4.0f * scale);
        size.z() = std::max(size.z(), 8.0f * scale);

        // Normalize the local bone direction
        osg::Vec3f boneDir = localBoneDir;
        float boneDirLen = boneDir.length();
        if (boneDirLen > 0.001f)
            boneDir /= boneDirLen;
        else
            boneDir = osg::Vec3f(0, 0, 1);  // Default to Z if no direction

        // Calculate shape offset position - center of shape along bone direction
        float boneLength = size.z();
        float centerOffset = boneLength * 0.5f;
        osg::Vec3f shapeCenter = boneDir * centerOffset;

        JPH::Ref<JPH::Shape> shape;

        switch (config.shapeType)
        {
            case JointConfig::ShapeType::Sphere:
            {
                float radius = std::max({size.x(), size.y(), size.z()}) * 0.4f * shapeScale;
                // Sphere is symmetric, just translate to center
                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    JPH::Quat::sIdentity(),
                    new JPH::SphereShape(radius));
                outShapeOffset = shapeCenter;
                break;
            }
            case JointConfig::ShapeType::Box:
            {
                float halfX = size.x() * 0.4f * shapeScale;
                float halfY = size.y() * 0.4f * shapeScale;
                float halfZ = size.z() * 0.4f * shapeScale;

                // Calculate rotation to align box's Z with local bone direction
                // We want the box's longest axis (Z) to align with boneDir
                osg::Quat boxRot;
                boxRot.makeRotate(osg::Vec3f(0, 0, 1), boneDir);

                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    Misc::Convert::toJolt(boxRot),
                    new JPH::BoxShape(JPH::Vec3(halfX, halfY, halfZ)));
                outShapeOffset = shapeCenter;
                break;
            }
            case JointConfig::ShapeType::Capsule:
            default:
            {
                float radius = std::min(size.x(), size.y()) * 0.3f * shapeScale;
                float halfHeight = (size.z() * 0.5f - radius) * shapeScale;
                if (halfHeight < 0.0f)
                {
                    halfHeight = 0.0f;
                    radius = size.z() * 0.4f * shapeScale;
                }

                // Jolt capsules are along Y by default
                // We need to rotate them to align with the bone direction in local space
                // First rotate from Y to the local bone direction

                // Calculate rotation from default capsule axis (Y) to local bone direction
                osg::Quat capsuleRot;
                capsuleRot.makeRotate(osg::Vec3f(0, 1, 0), boneDir);

                shape = new JPH::RotatedTranslatedShape(
                    Misc::Convert::toJolt<JPH::Vec3>(shapeCenter),
                    Misc::Convert::toJolt(capsuleRot),
                    new JPH::CapsuleShape(halfHeight, radius));
                outShapeOffset = shapeCenter;
                break;
            }
        }

        // CRITICAL: Wrap shape with OffsetCenterOfMassShape to ensure the center of mass
        // is at the body origin (joint position). This is necessary because:
        // 1. Body position is at the joint/bone origin
        // 2. Shape is offset from body origin using RotatedTranslatedShape
        // 3. Without this, the COM would be at the shape's geometric center, not the joint
        // 4. Constraints use LocalToBodyCOM, so they need COM at the joint position
        JPH::Vec3 comOffset = shape->GetCenterOfMass();
        if (comOffset.LengthSq() > 0.0001f)
        {
            // Shift COM back to origin
            shape = new JPH::OffsetCenterOfMassShape(shape, -comOffset);
        }

        return shape;
    }

    JPH::Ref<JPH::TwoBodyConstraintSettings> RagdollSettingsBuilder::createConstraintSettings(
        const osg::Matrix& parentWorldMatrix,
        const osg::Matrix& childWorldMatrix,
        const JointConfig& config)
    {
        auto settings = new JPH::SwingTwistConstraintSettings;

        // CRITICAL: Use LocalToBodyCOM space for ragdoll constraints
        // This mode expects positions and axes in each body's local coordinate frame
        // relative to its center of mass (which we've set to body origin via OffsetCenterOfMassShape)
        settings->mSpace = JPH::EConstraintSpace::LocalToBodyCOM;

        // Get world positions and rotations
        osg::Vec3f parentPos = parentWorldMatrix.getTrans();
        osg::Vec3f childPos = childWorldMatrix.getTrans();
        osg::Quat parentRot = parentWorldMatrix.getRotate();
        osg::Quat childRot = childWorldMatrix.getRotate();

        // The joint/constraint point is at the CHILD's bone origin (where it connects to parent)
        // This is the world-space location of the joint
        osg::Vec3f jointWorldPos = childPos;

        // For LocalToBodyCOM mode, we need to express the constraint position
        // in each body's LOCAL coordinate frame.
        //
        // Body positions are at bone origins (joint locations).
        // Parent body is at parentPos, child body is at childPos.
        // The constraint anchor for parent needs to be offset from parent's origin to the joint (childPos).
        // The constraint anchor for child is at its own origin (0,0,0 in local space).

        // Parent's local constraint position: transform joint world pos to parent's local space
        // parentLocalPos = inverse(parentRot) * (jointWorldPos - parentPos)
        osg::Vec3f parentToJoint = jointWorldPos - parentPos;
        osg::Vec3f parentLocalPos = parentRot.inverse() * parentToJoint;

        // Child's local constraint position: the joint is at child's origin, so (0,0,0)
        osg::Vec3f childLocalPos(0, 0, 0);

        settings->mPosition1 = JPH::RVec3(parentLocalPos.x(), parentLocalPos.y(), parentLocalPos.z());
        settings->mPosition2 = JPH::RVec3(childLocalPos.x(), childLocalPos.y(), childLocalPos.z());

        // Calculate the bone direction from parent to child in world space
        // This is the twist axis - rotation around this axis is "twist"
        osg::Vec3f boneDir = childPos - parentPos;
        float boneLength = boneDir.length();

        if (boneLength > 0.001f)
            boneDir /= boneLength;
        else
            boneDir = osg::Vec3f(0, 0, 1);  // Default to Z-up if bones overlap

        // For LocalToBodyCOM mode, axes must be in each body's local coordinate frame
        // Transform the world-space twist axis to each body's local space

        // Parent's local twist axis
        osg::Vec3f parentLocalTwist = parentRot.inverse() * boneDir;
        parentLocalTwist.normalize();

        // Child's local twist axis
        osg::Vec3f childLocalTwist = childRot.inverse() * boneDir;
        childLocalTwist.normalize();

        settings->mTwistAxis1 = Misc::Convert::toJolt<JPH::Vec3>(parentLocalTwist);
        settings->mTwistAxis2 = Misc::Convert::toJolt<JPH::Vec3>(childLocalTwist);

        // Plane axis must be perpendicular to twist axis
        // Use world up as reference, computing a proper perpendicular
        osg::Vec3f worldUp(0, 0, 1);
        if (std::abs(boneDir * worldUp) > 0.9f)
            worldUp = osg::Vec3f(1, 0, 0);  // Use X if bone is nearly vertical

        // Compute perpendicular using cross product in world space
        osg::Vec3f planeDir = boneDir ^ worldUp;
        planeDir.normalize();

        // Verify perpendicularity (should be very close to 0)
        float dotCheck = boneDir * planeDir;
        if (std::abs(dotCheck) > 0.01f)
        {
            // Force perpendicularity using Gram-Schmidt
            planeDir = planeDir - boneDir * dotCheck;
            planeDir.normalize();
        }

        // Transform plane axis to each body's local space
        osg::Vec3f parentLocalPlane = parentRot.inverse() * planeDir;
        parentLocalPlane.normalize();

        osg::Vec3f childLocalPlane = childRot.inverse() * planeDir;
        childLocalPlane.normalize();

        settings->mPlaneAxis1 = Misc::Convert::toJolt<JPH::Vec3>(parentLocalPlane);
        settings->mPlaneAxis2 = Misc::Convert::toJolt<JPH::Vec3>(childLocalPlane);

        // Apply joint limits
        // NormalHalfConeAngle: swing limit perpendicular to plane axis
        // PlaneHalfConeAngle: swing limit in the plane
        settings->mNormalHalfConeAngle = config.swingLimit;
        settings->mPlaneHalfConeAngle = config.swingLimit;
        settings->mTwistMinAngle = config.twistMinLimit;
        settings->mTwistMaxAngle = config.twistMaxLimit;

        return settings;
    }

    void RagdollSettingsBuilder::traverseSkeleton(
        osg::MatrixTransform* node,
        int parentJoltIndex,
        BuildContext& ctx)
    {
        std::string boneName = node->getName();
        std::string lowerName = Misc::StringUtils::lowerCase(boneName);

        // Only include bones from the physics whitelist
        // Skip geometry attachment points, equipment bones, and morph targets
        if (!isPhysicsBone(lowerName))
        {
            // Still traverse children - important physics bones may be below this node
            for (unsigned int i = 0; i < node->getNumChildren(); ++i)
            {
                auto* child = dynamic_cast<osg::MatrixTransform*>(node->getChild(i));
                if (child && isBone(child))
                    traverseSkeleton(child, parentJoltIndex, ctx);  // Pass SAME parent index
            }
            return;
        }

        // Get config for this bone
        JointConfig config = getDefaultConfig(boneName);
        if (ctx.overrides)
        {
            auto it = ctx.overrides->find(lowerName);
            if (it != ctx.overrides->end())
                config = it->second;
        }

        // Add joint to Jolt skeleton
        int joltIndex;
        if (parentJoltIndex < 0)
        {
            joltIndex = ctx.joltSkeleton->AddJoint(boneName.c_str());
        }
        else
        {
            joltIndex = ctx.joltSkeleton->AddJoint(boneName.c_str(), parentJoltIndex);
        }

        // Calculate world matrix
        osg::Matrix worldMatrix = getWorldMatrix(node);
        osg::Vec3f worldPos = worldMatrix.getTrans();
        osg::Quat worldRot = worldMatrix.getRotate();

        // Find child bone to determine bone direction
        osg::Vec3f boneDirection(0, 0, 1);  // Default direction
        osg::MatrixTransform* childBone = findFirstChildBone(node);
        if (childBone)
        {
            osg::Matrix childWorld = getWorldMatrix(childBone);
            osg::Vec3f childPos = childWorld.getTrans();
            boneDirection = childPos - worldPos;
            if (boneDirection.length() > 0.001f)
                boneDirection.normalize();
            else
                boneDirection = osg::Vec3f(0, 0, 1);
        }

        // Calculate local bone direction in native bone frame
        osg::Vec3f localBoneDir = worldRot.inverse() * boneDirection;

        // Estimate bone size
        osg::Vec3f boneSize = estimateBoneSize(node, ctx.scale);

        // Create shape
        osg::Vec3f shapeOffset;
        JPH::Ref<JPH::Shape> shape = createBoneShape(node, boneSize, config, ctx.scale, localBoneDir, shapeOffset);

        // Create body settings for this part
        // IMPORTANT: Body position is at the JOINT (bone origin), shape is offset
        JPH::RagdollSettings::Part& part = ctx.settings->mParts.emplace_back();

        part.SetShape(shape);
        part.mPosition = Misc::Convert::toJolt<JPH::RVec3>(worldPos);
        part.mRotation = Misc::Convert::toJolt(worldRot);
        part.mMotionType = JPH::EMotionType::Dynamic;
        part.mObjectLayer = Layers::DEBRIS;

        // Mass will be normalized later
        part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        part.mMassPropertiesOverride.mMass = config.mass;
        ctx.massSum += config.mass;

        // Damping for stable simulation
        part.mLinearDamping = 0.5f;
        part.mAngularDamping = 0.8f;
        part.mFriction = 0.8f;
        part.mRestitution = 0.0f;
        part.mMotionQuality = JPH::EMotionQuality::LinearCast;
        part.mAllowSleeping = true;
        part.mGravityFactor = 1.0f;

        // Create constraint to parent (if not root)
        if (parentJoltIndex >= 0)
        {
            // Get parent's world matrix from the OSG node we mapped
            // Find parent bone in mappings
            osg::Matrix parentWorld;
            bool foundParent = false;
            for (const auto& mapping : *ctx.mappings)
            {
                if (mapping.joltJointIndex == parentJoltIndex)
                {
                    parentWorld = getWorldMatrix(mapping.osgNode);
                    foundParent = true;

                    // Debug: log constraint setup
                    osg::Vec3f parentPos = parentWorld.getTrans();
                    Log(Debug::Verbose) << "  Constraint " << boneName << " -> parent " << mapping.boneName
                                        << ": parentPos=(" << parentPos.x() << "," << parentPos.y() << "," << parentPos.z() << ")"
                                        << " childPos=(" << worldPos.x() << "," << worldPos.y() << "," << worldPos.z() << ")";
                    break;
                }
            }

            if (!foundParent)
            {
                Log(Debug::Warning) << "RagdollSettingsBuilder: Could not find parent mapping for "
                                    << boneName << " (parentJoltIndex=" << parentJoltIndex << ")";
            }

            part.mToParent = createConstraintSettings(parentWorld, worldMatrix, config);
        }

        // Store mapping
        BoneMapping mapping;
        mapping.joltJointIndex = joltIndex;
        mapping.osgNode = node;
        mapping.shapeOffset = shapeOffset;
        mapping.boneName = boneName;
        ctx.mappings->push_back(mapping);

        // Traverse children
        for (unsigned int i = 0; i < node->getNumChildren(); ++i)
        {
            auto* child = dynamic_cast<osg::MatrixTransform*>(node->getChild(i));
            if (child && isBone(child))
                traverseSkeleton(child, joltIndex, ctx);
        }
    }

    JPH::Ref<JPH::RagdollSettings> RagdollSettingsBuilder::build(
        SceneUtil::Skeleton* osgSkeleton,
        float totalMass,
        float scale,
        const JointConfigMap* overrides,
        std::vector<BoneMapping>& outMappings)
    {
        if (!osgSkeleton)
        {
            Log(Debug::Error) << "RagdollSettingsBuilder: null skeleton";
            return nullptr;
        }

        outMappings.clear();

        // Step 1: Collect all bones from the OSG skeleton by name
        CollectBonesVisitor collector;
        osgSkeleton->accept(collector);

        std::unordered_map<std::string, osg::MatrixTransform*> bonesByName;
        for (auto* bone : collector.mBones)
        {
            std::string lowerName = Misc::StringUtils::lowerCase(bone->getName());
            bonesByName[lowerName] = bone;
        }

        Log(Debug::Verbose) << "RagdollSettingsBuilder: Found " << bonesByName.size() << " bones in skeleton";

        // Debug: Log all bone names found
        Log(Debug::Verbose) << "RagdollSettingsBuilder: Bone names in skeleton:";
        for (const auto& [name, node] : bonesByName)
        {
            Log(Debug::Verbose) << "  - '" << name << "'";
        }

        // Step 2: Build the ragdoll using our explicit physics hierarchy
        // This ensures correct anatomical parent-child relationships

        JPH::Ref<JPH::Skeleton> joltSkeleton = new JPH::Skeleton;
        JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;

        // Map from bone name to Jolt joint index
        std::unordered_map<std::string, int> boneToJoltIndex;

        // Process bones in correct order (parents before children)
        // Use a fixed order to ensure deterministic results
        const std::vector<std::string> processingOrder = {
            "bip01 pelvis",
            // Spine chain
            "bip01 spine",
            "bip01 spine1",
            "bip01 spine2",
            // Head
            "bip01 neck",
            "bip01 head",
            // Left arm
            "bip01 l clavicle",
            "bip01 l upperarm",
            "bip01 l forearm",
            "bip01 l hand",
            // Right arm
            "bip01 r clavicle",
            "bip01 r upperarm",
            "bip01 r forearm",
            "bip01 r hand",
            // Left leg
            "bip01 l thigh",
            "bip01 l calf",
            "bip01 l foot",
            // Right leg
            "bip01 r thigh",
            "bip01 r calf",
            "bip01 r foot",
        };

        float massSum = 0.0f;

        for (const std::string& boneName : processingOrder)
        {
            // Find the OSG node for this bone
            auto nodeIt = bonesByName.find(boneName);
            if (nodeIt == bonesByName.end())
            {
                Log(Debug::Verbose) << "RagdollSettingsBuilder: Bone not found in skeleton: " << boneName;
                continue;
            }

            osg::MatrixTransform* boneNode = nodeIt->second;

            // Get the physics parent
            std::string parentName = getPhysicsParent(boneName);
            int parentJoltIndex = -1;
            if (!parentName.empty())
            {
                auto parentIt = boneToJoltIndex.find(parentName);
                if (parentIt != boneToJoltIndex.end())
                {
                    parentJoltIndex = parentIt->second;
                }
                else
                {
                    Log(Debug::Warning) << "RagdollSettingsBuilder: Parent bone not yet processed: "
                                        << parentName << " for " << boneName;
                    continue;
                }
            }

            // Get config for this bone
            JointConfig config = getDefaultConfig(boneName);
            if (overrides)
            {
                auto it = overrides->find(boneName);
                if (it != overrides->end())
                    config = it->second;
            }

            // Add joint to Jolt skeleton
            int joltIndex;
            if (parentJoltIndex < 0)
            {
                joltIndex = joltSkeleton->AddJoint(boneName.c_str());
            }
            else
            {
                joltIndex = joltSkeleton->AddJoint(boneName.c_str(), parentJoltIndex);
            }
            boneToJoltIndex[boneName] = joltIndex;

            // Calculate world matrix
            osg::Matrix worldMatrix = getWorldMatrix(boneNode);
            osg::Vec3f worldPos = worldMatrix.getTrans();

            // Find the physics child to calculate bone direction
            // The bone direction is from this bone to its child (where the bone "points")
            osg::Vec3f boneDirection(0, 0, 1);  // Default to Z-up
            float boneLength = 15.0f * scale;   // Default length

            // Look for physics child in processingOrder
            std::string childName;
            for (const auto& [name, parent] : sPhysicsHierarchy)
            {
                if (parent == boneName)
                {
                    childName = name;
                    break;
                }
            }

            if (!childName.empty())
            {
                auto childNodeIt = bonesByName.find(childName);
                if (childNodeIt != bonesByName.end())
                {
                    osg::Matrix childWorld = getWorldMatrix(childNodeIt->second);
                    osg::Vec3f childPos = childWorld.getTrans();
                    boneDirection = childPos - worldPos;
                    boneLength = boneDirection.length();
                    if (boneLength > 0.001f)
                        boneDirection /= boneLength;
                    else
                        boneDirection = osg::Vec3f(0, 0, 1);
                }
            }
            else if (!parentName.empty())
            {
                // Leaf bone - use parent-to-this direction as bone direction
                auto parentNodeIt = bonesByName.find(parentName);
                if (parentNodeIt != bonesByName.end())
                {
                    osg::Matrix parentWorld = getWorldMatrix(parentNodeIt->second);
                    osg::Vec3f parentPos = parentWorld.getTrans();
                    boneDirection = worldPos - parentPos;
                    boneLength = boneDirection.length();
                    if (boneLength > 0.001f)
                        boneDirection /= boneLength;
                    else
                        boneDirection = osg::Vec3f(0, 0, 1);
                }
            }

            // CRITICAL FIX: Use native bone world rotation instead of calculated rotation
            // OpenMW animations use native bone rotations from NIF files. Using a calculated
            // rotation (makeRotationAlongZ) creates a mismatch that breaks constraints and
            // transform sync. By using the native rotation, physics bodies match OSG bones directly.
            osg::Quat bodyRotation = worldMatrix.getRotate();

            // Calculate the direction to child in the bone's native LOCAL frame
            // This is needed for shape offset calculation
            osg::Vec3f localBoneDir = bodyRotation.inverse() * boneDirection;

            // Estimate bone size using actual bone length
            osg::Vec3f boneSize(boneLength * 0.3f, boneLength * 0.3f, boneLength);

            // Create shape - offset along the bone direction in the bone's native local frame
            osg::Vec3f shapeOffset;
            JPH::Ref<JPH::Shape> shape = createBoneShape(boneNode, boneSize, config, scale, localBoneDir, shapeOffset);

            // Create body settings for this part
            JPH::RagdollSettings::Part& part = settings->mParts.emplace_back();

            part.SetShape(shape);
            part.mPosition = Misc::Convert::toJolt<JPH::RVec3>(worldPos);
            part.mRotation = Misc::Convert::toJolt(bodyRotation);  // Use calculated rotation, not bone's native rotation
            part.mMotionType = JPH::EMotionType::Dynamic;
            part.mObjectLayer = Layers::DEBRIS;

            if (boneName == "bip01 pelvis")
            {
                Log(Debug::Info) << "RAGDOLL BUILD [pelvis]: worldPos=("
                    << worldPos.x() << ", " << worldPos.y() << ", " << worldPos.z() << ")"
                    << " from getWorldMatrix";
            }

            part.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            part.mMassPropertiesOverride.mMass = config.mass;
            massSum += config.mass;

            // Damping for stable simulation
            part.mLinearDamping = 0.5f;
            part.mAngularDamping = 0.8f;
            part.mFriction = 0.8f;
            part.mRestitution = 0.0f;
            part.mMotionQuality = JPH::EMotionQuality::LinearCast;
            part.mAllowSleeping = true;
            part.mGravityFactor = 1.0f;

            // Create constraint to parent (if not root)
            if (parentJoltIndex >= 0 && !parentName.empty())
            {
                auto parentNodeIt = bonesByName.find(parentName);
                if (parentNodeIt != bonesByName.end())
                {
                    // Get parent's native world matrix directly from OSG - this is the key!
                    // We're now using native bone rotations for physics bodies, so we must
                    // use the same native rotations for constraint setup.
                    osg::Matrix parentWorld = getWorldMatrix(parentNodeIt->second);

                    float boneDistance = (worldPos - parentWorld.getTrans()).length();
                    Log(Debug::Verbose) << "  Constraint " << boneName << " -> parent " << parentName
                                        << ": boneLength=" << boneDistance
                                        << " boneDir=(" << boneDirection.x() << "," << boneDirection.y() << "," << boneDirection.z() << ")";

                    // Pass native bone world matrices directly - createConstraintSettings
                    // will extract native rotations and compute proper local-space axes
                    part.mToParent = createConstraintSettings(parentWorld, worldMatrix, config);
                }
            }

            // Store mapping
            BoneMapping mapping;
            mapping.joltJointIndex = joltIndex;
            mapping.osgNode = boneNode;
            mapping.shapeOffset = shapeOffset;
            mapping.boneName = boneName;
            mapping.physicsParentName = parentName;  // Store physics parent for transform sync
            mapping.boneDirection = boneDirection;
            mapping.bodyRotation = bodyRotation;
            mapping.originalBoneWorldRot = worldMatrix.getRotate();  // Store original bone rotation
            outMappings.push_back(mapping);
        }

        if (outMappings.empty())
        {
            Log(Debug::Warning) << "RagdollSettingsBuilder: No bones processed";
            return nullptr;
        }

        // Normalize masses so they sum to totalMass
        if (massSum > 0.0f)
        {
            float massScale = totalMass / massSum;
            for (auto& part : settings->mParts)
            {
                part.mMassPropertiesOverride.mMass *= massScale;
            }
        }

        // Assign skeleton to settings
        settings->mSkeleton = joltSkeleton;

        // Debug: Log all parts and their constraints
        Log(Debug::Info) << "RagdollSettingsBuilder: Parts created:";
        for (size_t i = 0; i < settings->mParts.size(); ++i)
        {
            const auto& part = settings->mParts[i];
            bool hasConstraint = (part.mToParent != nullptr);
            Log(Debug::Info) << "  Part " << i << " (" << outMappings[i].boneName << "): "
                             << "hasConstraint=" << hasConstraint;
        }

        // Debug: Log skeleton joints
        Log(Debug::Info) << "RagdollSettingsBuilder: Skeleton joints:";
        for (int i = 0; i < joltSkeleton->GetJointCount(); ++i)
        {
            const auto& joint = joltSkeleton->GetJoint(i);
            Log(Debug::Info) << "  Joint " << i << ": " << joint.mName.c_str()
                             << " parent=" << joint.mParentJointIndex;
        }

        // Calculate constraint indices for proper body relationships
        settings->CalculateBodyIndexToConstraintIndex();
        settings->CalculateConstraintIndexToBodyIdxPair();

        // Stabilize constraints
        if (!settings->Stabilize())
        {
            Log(Debug::Warning) << "RagdollSettingsBuilder: Failed to stabilize constraints";
            // Continue anyway, might still work
        }

        Log(Debug::Info) << "RagdollSettingsBuilder: Created settings with "
                         << outMappings.size() << " bones, "
                         << settings->mParts.size() << " parts";

        return settings;
    }
}
