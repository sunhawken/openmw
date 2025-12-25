#ifndef OPENMW_MWPHYSICS_SKELETONMAPPER_H
#define OPENMW_MWPHYSICS_SKELETONMAPPER_H

#include <osg/MatrixTransform>
#include <osg/Quat>
#include <osg/Vec3f>

#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonMapper.h>
#include <Jolt/Skeleton/SkeletonPose.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace SceneUtil
{
    class Skeleton;
}

namespace NifOsg
{
    class MatrixTransform;
}

namespace JPH
{
    class PhysicsSystem;
}

namespace MWPhysics
{
    /// Information about an OSG bone for mapping
    struct OsgBoneInfo
    {
        osg::MatrixTransform* node = nullptr;
        std::string name;
        int osgParentIndex = -1;        // Index in the OsgBoneInfo vector
        int joltMappedIndex = -1;       // Corresponding Jolt skeleton joint index (-1 if unmapped)
        JPH::Mat44 bindPoseModelSpace;  // Bind pose in model space (skeleton root space)
        JPH::Mat44 bindPoseLocal;       // Bind pose in local space (relative to parent)
    };

    /// Manages the mapping between OSG animation skeleton and Jolt ragdoll skeleton.
    /// Uses Jolt's SkeletonMapper for proper interpolation of unmapped joints.
    class RagdollSkeletonMapper
    {
    public:
        RagdollSkeletonMapper();
        ~RagdollSkeletonMapper();

        /// Initialize the mapper with both skeletons
        /// @param osgSkeleton The OSG animation skeleton (high-detail)
        /// @param joltSkeleton The Jolt ragdoll skeleton (low-detail)
        /// @param ragdoll The created ragdoll instance
        /// @param physicsSystem The Jolt physics system (for body interface access)
        /// @param skeletonRootTransform World transform of the skeleton root
        /// @return true if initialization succeeded
        bool initialize(
            SceneUtil::Skeleton* osgSkeleton,
            const JPH::Skeleton* joltSkeleton,
            JPH::Ragdoll* ragdoll,
            JPH::PhysicsSystem* physicsSystem,
            const osg::Matrix& skeletonRootTransform
        );

        /// Map ragdoll physics poses back to OSG skeleton transforms.
        /// Call this each frame to update the visual mesh.
        void mapRagdollToOsg();

        /// Map OSG animation poses to ragdoll (for powered ragdoll / animation blending)
        /// @param blendWeight How much animation vs physics (0 = full physics, 1 = full animation)
        void mapOsgToRagdoll(float blendWeight = 1.0f);

        /// Get the current root position from physics
        osg::Vec3f getRootPosition() const;

        /// Get the current root rotation from physics
        osg::Quat getRootRotation() const;

        /// Check if the mapper is properly initialized
        bool isValid() const { return mIsValid; }

        /// Get bone info for debugging
        const std::vector<OsgBoneInfo>& getOsgBones() const { return mOsgBones; }

    private:
        /// Collect all bones from the OSG skeleton
        void collectOsgBones(SceneUtil::Skeleton* skeleton, const osg::Matrix& skeletonRootTransform);

        /// Build the Jolt animation skeleton that mirrors OSG structure
        void buildAnimationSkeleton();

        /// Compute neutral poses for both skeletons
        void computeNeutralPoses();

        /// Apply a model-space transform to an OSG bone
        void applyModelSpaceTransform(size_t boneIndex, const JPH::Mat44& modelSpaceTransform);

        // OSG skeleton data
        std::vector<OsgBoneInfo> mOsgBones;
        std::unordered_map<std::string, size_t> mOsgBonesByName;
        osg::Matrix mSkeletonRootTransform;
        osg::Matrix mSkeletonRootInverse;

        // Jolt animation skeleton (mirrors OSG structure for SkeletonMapper)
        JPH::Ref<JPH::Skeleton> mAnimationSkeleton;
        std::vector<JPH::Mat44> mAnimationNeutralPose;  // Model space
        std::vector<JPH::Mat44> mAnimationLocalPose;    // Local space

        // Jolt ragdoll skeleton (low-detail physics skeleton)
        const JPH::Skeleton* mRagdollSkeleton = nullptr;
        std::vector<JPH::Mat44> mRagdollNeutralPose;  // Model space
        JPH::Ragdoll* mRagdoll = nullptr;
        JPH::PhysicsSystem* mPhysicsSystem = nullptr;

        // The mapper itself
        JPH::SkeletonMapper mMapper;

        // Working buffers for mapping
        mutable std::vector<JPH::Mat44> mRagdollPoseBuffer;
        mutable std::vector<JPH::Mat44> mAnimationPoseBuffer;
        mutable std::vector<JPH::Mat44> mAnimationLocalBuffer;

        bool mIsValid = false;

        // Root bone tracking
        size_t mRootOsgBoneIndex = 0;
        int mRootRagdollJointIndex = 0;
        osg::Vec3f mInitialRootOffset;  // World position of ragdoll root at initialization

        // Index mappings between OSG bone order and Jolt animation skeleton order
        std::vector<int> mOsgToAnimIndex;       // OSG bone index -> Jolt animation joint index
        std::unordered_map<int, size_t> mAnimToOsgIndex;  // Jolt animation joint index -> OSG bone index
    };

}  // namespace MWPhysics

#endif  // OPENMW_MWPHYSICS_SKELETONMAPPER_H
