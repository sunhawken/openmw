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

    /// Information about a bone used during ragdoll construction
    struct BoneData
    {
        osg::MatrixTransform* osgNode = nullptr;
        std::string name;
        std::string physicsParentName;
        int joltJointIndex = -1;
        osg::Vec3f worldPosition;
        osg::Quat worldRotation;
        osg::Vec3f boneDirection;  // Direction to child bone (normalized, world space)
        float boneLength = 15.0f;
    };

    /// Builds JPH::RagdollSettings from an OSG skeleton hierarchy.
    ///
    /// This builder creates a physics-only ragdoll skeleton. The mapping between
    /// the physics skeleton and the full animation skeleton is handled separately
    /// by RagdollSkeletonMapper.
    class RagdollSettingsBuilder
    {
    public:
        using JointConfigMap = std::unordered_map<std::string, JointConfig>;

        /// Build ragdoll settings from an OSG skeleton
        /// @param osgSkeleton The OSG skeleton to build from
        /// @param totalMass Total mass of the ragdoll in kg
        /// @param scale Actor scale factor
        /// @param overrides Optional per-bone configuration overrides (by lowercase bone name)
        /// @return RagdollSettings ready to create a Ragdoll, or nullptr on failure
        static JPH::Ref<JPH::RagdollSettings> build(
            SceneUtil::Skeleton* osgSkeleton,
            float totalMass,
            float scale,
            const JointConfigMap* overrides = nullptr
        );

        /// Get default joint config for common bone types based on name heuristics
        static JointConfig getDefaultConfig(const std::string& boneName);

        /// Get the list of physics bone names (for reference)
        static const std::vector<std::string>& getPhysicsBoneNames();

    private:
        /// Create collision shape for a bone
        static JPH::Ref<JPH::Shape> createBoneShape(
            const BoneData& bone,
            const JointConfig& config,
            float scale
        );

        /// Create constraint settings for a joint
        static JPH::Ref<JPH::TwoBodyConstraintSettings> createConstraintSettings(
            const BoneData& parentBone,
            const BoneData& childBone,
            const JointConfig& config
        );

        /// Get world matrix for a node
        static osg::Matrix getWorldMatrix(osg::Node* node);

        /// Check if a bone name is in our physics skeleton
        static bool isPhysicsBone(const std::string& lowerName);

        /// Get the physics parent for a bone
        static std::string getPhysicsParent(const std::string& lowerName);
    };
}

#endif
