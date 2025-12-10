#ifndef OPENMW_MWPHYSICS_RAGDOLLBUILDER_H
#define OPENMW_MWPHYSICS_RAGDOLLBUILDER_H

#include <osg/MatrixTransform>
#include <osg/Vec3f>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Skeleton/Skeleton.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace SceneUtil
{
    class Skeleton;
}

namespace MWPhysics
{
    /// Configuration for a single joint in the ragdoll
    struct JointConfig
    {
        float mass = 1.0f;              // Relative mass (will be normalized)
        float swingLimit = 0.5f;        // Swing cone half-angle in radians
        float twistMinLimit = -0.3f;    // Twist min angle in radians
        float twistMaxLimit = 0.3f;     // Twist max angle in radians

        enum class ShapeType { Capsule, Box, Sphere };
        ShapeType shapeType = ShapeType::Capsule;

        // Shape scale multiplier (1.0 = full bone length)
        float shapeScale = 0.7f;
    };

    /// Mapping between Jolt skeleton joint and OSG bone node
    struct BoneMapping
    {
        int joltJointIndex;
        osg::MatrixTransform* osgNode;
        osg::Vec3f shapeOffset;  // Offset from joint origin to shape center (in bone local space)
        std::string boneName;
        osg::Vec3f boneDirection;  // Direction the bone points (normalized, world space at creation)
        osg::Quat bodyRotation;    // Rotation used for physics body (aligns Z with bone direction)
        osg::Quat originalBoneWorldRot;  // Original OSG bone world rotation at ragdoll creation
    };

    /// Builds JPH::RagdollSettings from an OSG skeleton hierarchy
    /// This is rig-agnostic - it works with any skeleton structure
    class RagdollSettingsBuilder
    {
    public:
        using JointConfigMap = std::unordered_map<std::string, JointConfig>;

        /// Build ragdoll settings from an OSG skeleton
        /// @param osgSkeleton The OSG skeleton to build from
        /// @param totalMass Total mass of the ragdoll in kg
        /// @param scale Actor scale factor
        /// @param overrides Optional per-bone configuration overrides (by lowercase bone name)
        /// @param outMappings Output vector of bone mappings for transform sync
        /// @return RagdollSettings ready to create a Ragdoll, or nullptr on failure
        static JPH::Ref<JPH::RagdollSettings> build(
            SceneUtil::Skeleton* osgSkeleton,
            float totalMass,
            float scale,
            const JointConfigMap* overrides,
            std::vector<BoneMapping>& outMappings
        );

        /// Get default joint config for common bone types based on name heuristics
        static JointConfig getDefaultConfig(const std::string& boneName);

    private:
        struct BuildContext
        {
            JPH::Skeleton* joltSkeleton;
            JPH::RagdollSettings* settings;
            std::vector<BoneMapping>* mappings;
            const JointConfigMap* overrides;
            float totalMass;
            float scale;
            float massSum;  // For normalization
        };

        /// Recursively traverse OSG skeleton to build Jolt skeleton and parts
        static void traverseSkeleton(
            osg::MatrixTransform* node,
            int parentJoltIndex,
            BuildContext& ctx
        );

        /// Create collision shape for a bone
        static JPH::Ref<JPH::Shape> createBoneShape(
            osg::MatrixTransform* bone,
            const osg::Vec3f& boneSize,
            const JointConfig& config,
            float scale,
            const osg::Vec3f& localBoneDir,
            osg::Vec3f& outShapeOffset
        );

        /// Estimate bone size from parent-child distance
        static osg::Vec3f estimateBoneSize(
            osg::MatrixTransform* bone,
            float scale
        );

        /// Create constraint settings for a joint
        static JPH::Ref<JPH::TwoBodyConstraintSettings> createConstraintSettings(
            const osg::Matrix& parentWorldMatrix,
            const osg::Matrix& childWorldMatrix,
            const JointConfig& config
        );

        /// Check if a node is a bone (MatrixTransform with a name)
        static bool isBone(osg::Node* node);

        /// Get world matrix for a node
        static osg::Matrix getWorldMatrix(osg::Node* node);

        /// Find first child bone of a node
        static osg::MatrixTransform* findFirstChildBone(osg::MatrixTransform* node);
    };
}

#endif
