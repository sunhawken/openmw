#include "skeletonmapper.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <osg/NodeVisitor>

#include <algorithm>

namespace MWPhysics
{
    namespace
    {
        // Visitor to collect all bone nodes from OSG skeleton
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

        // Convert OSG matrix to Jolt Mat44
        // Both OSG and Jolt use (row, col) accessors that return the mathematical element at row, col.
        // OSG stores row-major, Jolt stores column-major, but the accessors abstract this away.
        // We can use sRotationTranslation for cleaner conversion that handles the conventions properly.
        JPH::Mat44 toJoltMat44(const osg::Matrix& m)
        {
            // Use rotation/translation decomposition for robustness
            osg::Quat rot = m.getRotate();
            osg::Vec3f trans = m.getTrans();
            return JPH::Mat44::sRotationTranslation(
                Misc::Convert::toJolt(rot),
                Misc::Convert::toJolt<JPH::Vec3>(trans)
            );
        }

        // Convert Jolt Mat44 to OSG matrix
        // Use rotation/translation decomposition to ensure consistency with toJoltMat44
        osg::Matrix toOsgMatrix(const JPH::Mat44& m)
        {
            osg::Matrix result;
            JPH::Quat rot = m.GetRotation().GetQuaternion();
            JPH::Vec3 trans = m.GetTranslation();
            result.makeRotate(Misc::Convert::toOsg(rot));
            result.setTrans(Misc::Convert::toOsg(trans));
            return result;
        }

        // Get world matrix for an OSG node
        osg::Matrix getWorldMatrix(osg::Node* node)
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
    }

    RagdollSkeletonMapper::RagdollSkeletonMapper() = default;
    RagdollSkeletonMapper::~RagdollSkeletonMapper() = default;

    bool RagdollSkeletonMapper::initialize(
        SceneUtil::Skeleton* osgSkeleton,
        const JPH::Skeleton* joltSkeleton,
        JPH::Ragdoll* ragdoll,
        JPH::PhysicsSystem* physicsSystem,
        const osg::Matrix& skeletonRootTransform)
    {
        if (!osgSkeleton || !joltSkeleton || !ragdoll || !physicsSystem)
        {
            Log(Debug::Error) << "RagdollSkeletonMapper: Invalid input parameters";
            return false;
        }

        mRagdollSkeleton = joltSkeleton;
        mRagdoll = ragdoll;
        mPhysicsSystem = physicsSystem;
        mSkeletonRootTransform = skeletonRootTransform;
        mSkeletonRootInverse.invert(skeletonRootTransform);

        // Step 1: Collect all OSG bones
        collectOsgBones(osgSkeleton, skeletonRootTransform);

        if (mOsgBones.empty())
        {
            Log(Debug::Error) << "RagdollSkeletonMapper: No bones found in OSG skeleton";
            return false;
        }

        Log(Debug::Info) << "RagdollSkeletonMapper: Collected " << mOsgBones.size() << " OSG bones";

        // Step 2: Build the animation skeleton that mirrors OSG structure
        buildAnimationSkeleton();

        // Step 3: Compute neutral poses for both skeletons
        computeNeutralPoses();

        // Step 4: Initialize the Jolt SkeletonMapper
        // SkeletonMapper maps from skeleton1 (ragdoll, low-detail) to skeleton2 (animation, high-detail)
        mMapper.Initialize(
            mRagdollSkeleton,
            mRagdollNeutralPose.data(),
            mAnimationSkeleton,
            mAnimationNeutralPose.data()
        );

        // Step 5: Lock translations to prevent stretching
        mMapper.LockAllTranslations(mAnimationSkeleton, mAnimationNeutralPose.data());

        // Allocate working buffers
        mRagdollPoseBuffer.resize(mRagdollSkeleton->GetJointCount());
        mAnimationPoseBuffer.resize(mAnimationSkeleton->GetJointCount());
        mAnimationLocalBuffer.resize(mAnimationSkeleton->GetJointCount());

        // Log mapping info
        Log(Debug::Info) << "RagdollSkeletonMapper: Initialized with "
                         << mMapper.GetMappings().size() << " direct mappings, "
                         << mMapper.GetChains().size() << " chains, "
                         << mMapper.GetUnmapped().size() << " unmapped joints";

        // Log neutral pose comparison for debugging
        logNeutralPoseComparison();

        // Extra debug: log the ragdoll root body's actual transform
        if (mRootRagdollJointIndex >= 0)
        {
            JPH::BodyID rootBodyId = mRagdoll->GetBodyID(mRootRagdollJointIndex);
            if (!rootBodyId.IsInvalid())
            {
                JPH::BodyInterface& bodyInterface = mPhysicsSystem->GetBodyInterface();
                JPH::RVec3 bodyPos;
                JPH::Quat bodyRot;
                bodyInterface.GetPositionAndRotation(rootBodyId, bodyPos, bodyRot);
                Log(Debug::Info) << "Ragdoll root body actual transform: pos=("
                                 << bodyPos.GetX() << "," << bodyPos.GetY() << "," << bodyPos.GetZ()
                                 << ") rot=(" << bodyRot.GetX() << "," << bodyRot.GetY() << ","
                                 << bodyRot.GetZ() << "," << bodyRot.GetW() << ")";
            }
        }

        // Debug: verify that mapped joints have matching positions in neutral poses
        for (const auto& mapping : mMapper.GetMappings())
        {
            JPH::Vec3 ragdollPos = mRagdollNeutralPose[mapping.mJointIdx1].GetTranslation();
            JPH::Vec3 animPos = mAnimationNeutralPose[mapping.mJointIdx2].GetTranslation();
            float dist = (ragdollPos - animPos).Length();

            Log(Debug::Verbose) << "  Mapping: " << mRagdollSkeleton->GetJoint(mapping.mJointIdx1).mName
                                << " -> " << mAnimationSkeleton->GetJoint(mapping.mJointIdx2).mName
                                << " pos_dist=" << dist;

            if (dist > 5.0f)
            {
                Log(Debug::Warning) << "    Large position mismatch! ragdoll=("
                                    << ragdollPos.GetX() << "," << ragdollPos.GetY() << "," << ragdollPos.GetZ() << ")"
                                    << " anim=(" << animPos.GetX() << "," << animPos.GetY() << "," << animPos.GetZ() << ")";
            }
        }

        mIsValid = true;
        return true;
    }

    void RagdollSkeletonMapper::collectOsgBones(SceneUtil::Skeleton* skeleton, const osg::Matrix& skeletonRootTransform)
    {
        mOsgBones.clear();
        mOsgBonesByName.clear();

        CollectBonesVisitor collector;
        skeleton->accept(collector);

        // First pass: create bone info entries
        for (auto* bone : collector.mBones)
        {
            OsgBoneInfo info;
            info.node = bone;
            info.name = bone->getName();

            size_t index = mOsgBones.size();
            mOsgBonesByName[Misc::StringUtils::lowerCase(info.name)] = index;
            mOsgBones.push_back(info);
        }

        // Second pass: establish parent relationships and compute bind poses
        for (size_t i = 0; i < mOsgBones.size(); ++i)
        {
            OsgBoneInfo& info = mOsgBones[i];

            // Find parent
            if (info.node->getNumParents() > 0)
            {
                osg::Node* parent = info.node->getParent(0);
                if (parent)
                {
                    std::string parentName = Misc::StringUtils::lowerCase(parent->getName());
                    auto it = mOsgBonesByName.find(parentName);
                    if (it != mOsgBonesByName.end())
                    {
                        info.osgParentIndex = static_cast<int>(it->second);
                    }
                }
            }

            // Compute bind pose in model space (relative to skeleton root)
            osg::Matrix worldMatrix = getWorldMatrix(info.node);
            osg::Matrix modelMatrix = worldMatrix * mSkeletonRootInverse;
            info.bindPoseModelSpace = toJoltMat44(modelMatrix);

            // Compute local bind pose
            if (info.osgParentIndex >= 0)
            {
                const OsgBoneInfo& parentInfo = mOsgBones[info.osgParentIndex];
                JPH::Mat44 parentInverse = parentInfo.bindPoseModelSpace.Inversed();
                info.bindPoseLocal = parentInverse * info.bindPoseModelSpace;
            }
            else
            {
                info.bindPoseLocal = info.bindPoseModelSpace;
            }
        }

        // Third pass: find which OSG bones correspond to ragdoll joints
        for (int j = 0; j < mRagdollSkeleton->GetJointCount(); ++j)
        {
            std::string jointName = Misc::StringUtils::lowerCase(mRagdollSkeleton->GetJoint(j).mName.c_str());
            auto it = mOsgBonesByName.find(jointName);
            if (it != mOsgBonesByName.end())
            {
                mOsgBones[it->second].joltMappedIndex = j;
                Log(Debug::Verbose) << "  Mapped: " << jointName << " (OSG " << it->second << " <-> Jolt " << j << ")";
            }
        }

        // Find root bone (first bone with a jolt mapping, typically pelvis)
        for (size_t i = 0; i < mOsgBones.size(); ++i)
        {
            if (mOsgBones[i].joltMappedIndex >= 0)
            {
                // Check if this is the ragdoll root (joint with no parent in ragdoll)
                int joltIdx = mOsgBones[i].joltMappedIndex;
                if (mRagdollSkeleton->GetJoint(joltIdx).mParentJointIndex == -1)
                {
                    mRootOsgBoneIndex = i;
                    mRootRagdollJointIndex = joltIdx;
                    Log(Debug::Info) << "RagdollSkeletonMapper: Root bone is '"
                                     << mOsgBones[i].name << "' (OSG " << i << ", Jolt " << joltIdx << ")";

                    // Store Bip01's bind pose for later use (to avoid feedback loops)
                    OsgBoneInfo& rootBone = mOsgBones[i];
                    if (rootBone.node->getNumParents() > 0)
                    {
                        osg::Node* osgParent = rootBone.node->getParent(0);
                        auto* bip01 = dynamic_cast<osg::MatrixTransform*>(osgParent);
                        if (bip01)
                        {
                            mBip01BindPoseLocal = bip01->getMatrix();
                            mPelvisBindPoseLocalPos = rootBone.node->getMatrix().getTrans();

                            // Find Bip01's OSG index for later reference
                            std::string bip01Name = Misc::StringUtils::lowerCase(bip01->getName());
                            auto bip01It = mOsgBonesByName.find(bip01Name);
                            if (bip01It != mOsgBonesByName.end())
                            {
                                mBip01OsgIndex = static_cast<int>(bip01It->second);
                            }

                            Log(Debug::Info) << "RagdollSkeletonMapper: Stored Bip01 bind pose, pelvis offset = ("
                                             << mPelvisBindPoseLocalPos.x() << ", "
                                             << mPelvisBindPoseLocalPos.y() << ", "
                                             << mPelvisBindPoseLocalPos.z() << "), Bip01 OSG index = " << mBip01OsgIndex;
                        }
                    }
                    break;
                }
            }
        }
    }

    void RagdollSkeletonMapper::buildAnimationSkeleton()
    {
        // Create a Jolt skeleton that mirrors the OSG bone hierarchy
        // This is the "high-detail" skeleton for the mapper
        mAnimationSkeleton = new JPH::Skeleton;

        // Jolt requires joints in parent-before-child order
        // The OSG visitor may not collect them in this order, so we need to sort

        // Build a list of bones with their depth in the hierarchy
        struct BoneWithDepth
        {
            size_t osgIndex;
            int depth;
        };
        std::vector<BoneWithDepth> sortedBones;
        sortedBones.reserve(mOsgBones.size());

        for (size_t i = 0; i < mOsgBones.size(); ++i)
        {
            int depth = 0;
            int parentIdx = mOsgBones[i].osgParentIndex;
            while (parentIdx >= 0 && depth < 100)  // depth limit to prevent infinite loops
            {
                depth++;
                parentIdx = mOsgBones[parentIdx].osgParentIndex;
            }
            sortedBones.push_back({i, depth});
        }

        // Sort by depth (parents before children)
        std::sort(sortedBones.begin(), sortedBones.end(),
            [](const BoneWithDepth& a, const BoneWithDepth& b) {
                return a.depth < b.depth;
            });

        // Map from OSG bone index to Jolt animation skeleton index
        mOsgToAnimIndex.resize(mOsgBones.size(), -1);

        for (const auto& boneWithDepth : sortedBones)
        {
            size_t osgIdx = boneWithDepth.osgIndex;
            const OsgBoneInfo& info = mOsgBones[osgIdx];

            int parentJoltIndex = -1;
            if (info.osgParentIndex >= 0)
            {
                parentJoltIndex = mOsgToAnimIndex[info.osgParentIndex];
            }

            // Use lowercase name to match ragdoll skeleton naming convention
            std::string lowerName = Misc::StringUtils::lowerCase(info.name);

            int joltIndex;
            if (parentJoltIndex >= 0)
            {
                joltIndex = mAnimationSkeleton->AddJoint(lowerName.c_str(), parentJoltIndex);
            }
            else
            {
                joltIndex = mAnimationSkeleton->AddJoint(lowerName.c_str());
            }

            mOsgToAnimIndex[osgIdx] = joltIndex;
            mAnimToOsgIndex[joltIndex] = osgIdx;
        }

        Log(Debug::Info) << "RagdollSkeletonMapper: Built animation skeleton with "
                         << mAnimationSkeleton->GetJointCount() << " joints";

        // Verify ordering
        if (!mAnimationSkeleton->AreJointsCorrectlyOrdered())
        {
            Log(Debug::Error) << "RagdollSkeletonMapper: Animation skeleton joints not correctly ordered!";
        }
    }

    void RagdollSkeletonMapper::computeNeutralPoses()
    {
        // CRITICAL: The SkeletonMapper requires that mapped joints have IDENTICAL neutral poses.
        // We build BOTH neutral poses from OSG world transforms to ensure consistency.
        // Using GetPose() for ragdoll could differ if bodies have been moved by physics.

        // Find the ragdoll root bone's OSG world position - this is our reference point
        osg::Vec3f rootWorldPos;
        if (mRootOsgBoneIndex < mOsgBones.size())
        {
            osg::Matrix rootWorldMat = getWorldMatrix(mOsgBones[mRootOsgBoneIndex].node);
            rootWorldPos = rootWorldMat.getTrans();
        }
        mInitialRootOffset = rootWorldPos;

        Log(Debug::Info) << "computeNeutralPoses: using OSG root position = ("
                         << rootWorldPos.x() << ", " << rootWorldPos.y() << ", " << rootWorldPos.z() << ")";

        // Build animation skeleton neutral pose from OSG
        mAnimationNeutralPose.resize(mAnimationSkeleton->GetJointCount());
        mAnimationLocalPose.resize(mAnimationSkeleton->GetJointCount());

        for (size_t osgIdx = 0; osgIdx < mOsgBones.size(); ++osgIdx)
        {
            int animIdx = mOsgToAnimIndex[osgIdx];
            if (animIdx >= 0 && animIdx < static_cast<int>(mAnimationNeutralPose.size()))
            {
                // Get world transform from OSG
                osg::Matrix worldMat = getWorldMatrix(mOsgBones[osgIdx].node);
                osg::Vec3f worldPos = worldMat.getTrans();
                osg::Quat worldRot = worldMat.getRotate();

                // Convert to model space: translation relative to root
                osg::Vec3f modelPos = worldPos - rootWorldPos;

                // Build model-space matrix
                JPH::Mat44 modelMat = JPH::Mat44::sRotationTranslation(
                    Misc::Convert::toJolt(worldRot),
                    Misc::Convert::toJolt<JPH::Vec3>(modelPos)
                );

                mAnimationNeutralPose[animIdx] = modelMat;

                // Store local pose for unmapped joint interpolation
                osg::Matrix localMat = mOsgBones[osgIdx].node->getMatrix();
                mAnimationLocalPose[animIdx] = toJoltMat44(localMat);
            }
        }

        // Build ragdoll skeleton neutral pose from the SAME OSG source
        // This ensures mapped joints have identical neutral poses
        mRagdollNeutralPose.resize(mRagdollSkeleton->GetJointCount());

        for (int ragIdx = 0; ragIdx < mRagdollSkeleton->GetJointCount(); ++ragIdx)
        {
            std::string jointName = Misc::StringUtils::lowerCase(mRagdollSkeleton->GetJoint(ragIdx).mName.c_str());
            auto it = mOsgBonesByName.find(jointName);
            if (it != mOsgBonesByName.end())
            {
                size_t osgIdx = it->second;
                osg::Matrix worldMat = getWorldMatrix(mOsgBones[osgIdx].node);
                osg::Vec3f worldPos = worldMat.getTrans();
                osg::Quat worldRot = worldMat.getRotate();

                // Convert to model space: translation relative to root
                osg::Vec3f modelPos = worldPos - rootWorldPos;

                mRagdollNeutralPose[ragIdx] = JPH::Mat44::sRotationTranslation(
                    Misc::Convert::toJolt(worldRot),
                    Misc::Convert::toJolt<JPH::Vec3>(modelPos)
                );

                // Debug: log ragdoll joints
                Log(Debug::Verbose) << "  Ragdoll joint " << ragIdx << " (" << jointName << ")"
                                    << ": modelPos=(" << modelPos.x() << "," << modelPos.y() << "," << modelPos.z() << ")"
                                    << " worldRot=(" << worldRot.x() << "," << worldRot.y() << "," << worldRot.z() << "," << worldRot.w() << ")";
            }
            else
            {
                // Fallback to identity if bone not found (shouldn't happen)
                Log(Debug::Warning) << "computeNeutralPoses: ragdoll joint " << jointName << " not found in OSG!";
                mRagdollNeutralPose[ragIdx] = JPH::Mat44::sIdentity();
            }
        }

        Log(Debug::Info) << "RagdollSkeletonMapper: Computed neutral poses from OSG (root offset: "
                         << mInitialRootOffset.x() << ", " << mInitialRootOffset.y() << ", "
                         << mInitialRootOffset.z() << ")";
    }

    void RagdollSkeletonMapper::logNeutralPoseComparison()
    {
        // DEBUG: Compare neutral poses for mapped joints
        Log(Debug::Info) << "=== NEUTRAL POSE COMPARISON ===";

        // First log ragdoll skeleton structure
        Log(Debug::Info) << "Ragdoll skeleton (" << mRagdollSkeleton->GetJointCount() << " joints):";
        for (int i = 0; i < mRagdollSkeleton->GetJointCount(); ++i)
        {
            const auto& joint = mRagdollSkeleton->GetJoint(i);
            Log(Debug::Info) << "  [" << i << "] " << joint.mName << " parent=" << joint.mParentJointIndex;
        }

        // Then log animation skeleton structure
        Log(Debug::Info) << "Animation skeleton (" << mAnimationSkeleton->GetJointCount() << " joints):";
        for (int i = 0; i < mAnimationSkeleton->GetJointCount(); ++i)
        {
            const auto& joint = mAnimationSkeleton->GetJoint(i);
            Log(Debug::Info) << "  [" << i << "] " << joint.mName << " parent=" << joint.mParentJointIndex;
        }

        // Compare poses for mapped joints
        Log(Debug::Info) << "Mapped joints comparison:";
        for (const auto& mapping : mMapper.GetMappings())
        {
            int ragIdx = mapping.mJointIdx1;
            int animIdx = mapping.mJointIdx2;

            JPH::Vec3 ragPos = mRagdollNeutralPose[ragIdx].GetTranslation();
            JPH::Quat ragRot = mRagdollNeutralPose[ragIdx].GetRotation().GetQuaternion();

            JPH::Vec3 animPos = mAnimationNeutralPose[animIdx].GetTranslation();
            JPH::Quat animRot = mAnimationNeutralPose[animIdx].GetRotation().GetQuaternion();

            // Calculate rotation difference
            JPH::Quat rotDiff = ragRot.Conjugated() * animRot;
            float angleDiff = 2.0f * std::acos(std::min(1.0f, std::abs(rotDiff.GetW()))) * 180.0f / 3.14159f;

            Log(Debug::Info) << "  " << mRagdollSkeleton->GetJoint(ragIdx).mName
                             << " [rag" << ragIdx << "] -> " << mAnimationSkeleton->GetJoint(animIdx).mName << " [anim" << animIdx << "]"
                             << "\n    Ragdoll: pos=(" << ragPos.GetX() << "," << ragPos.GetY() << "," << ragPos.GetZ()
                             << ") rot=(" << ragRot.GetX() << "," << ragRot.GetY() << "," << ragRot.GetZ() << "," << ragRot.GetW() << ")"
                             << "\n    Anim:    pos=(" << animPos.GetX() << "," << animPos.GetY() << "," << animPos.GetZ()
                             << ") rot=(" << animRot.GetX() << "," << animRot.GetY() << "," << animRot.GetZ() << "," << animRot.GetW() << ")"
                             << "\n    Rotation diff: " << angleDiff << " degrees";
        }
        Log(Debug::Info) << "=== END NEUTRAL POSE COMPARISON ===";
    }

    void RagdollSkeletonMapper::mapRagdollToOsg()
    {
        if (!mIsValid)
            return;

        // Step 1: Get current ragdoll pose using Jolt's GetPose
        // GetPose returns model-space poses (translations relative to root position, world rotations)
        JPH::RVec3 rootOffset;
        mRagdoll->GetPose(rootOffset, mRagdollPoseBuffer.data());
        osg::Vec3f rootWorldPos = Misc::Convert::toOsg(rootOffset);

        // Step 2: Use the stored neutral local poses for unmapped joints
        // IMPORTANT: Do NOT read current OSG transforms here - that causes a feedback loop
        // where each frame's output becomes the next frame's input, causing exponential explosion.
        // Instead, use the bind pose local transforms that were captured during initialization.
        for (size_t i = 0; i < mAnimationLocalPose.size(); ++i)
        {
            mAnimationLocalBuffer[i] = mAnimationLocalPose[i];
        }

        // Step 3: Use SkeletonMapper to map ragdoll -> animation
        mMapper.Map(
            mRagdollPoseBuffer.data(),
            mAnimationLocalBuffer.data(),
            mAnimationPoseBuffer.data()
        );

        // Debug: Check for NaN/invalid values in output
        static int debugFrameCount = 0;
        if (debugFrameCount++ < 5)  // Only log first 5 frames
        {
            for (int i = 0; i < mAnimationSkeleton->GetJointCount(); ++i)
            {
                JPH::Vec3 trans = mAnimationPoseBuffer[i].GetTranslation();
                if (std::isnan(trans.GetX()) || std::isnan(trans.GetY()) || std::isnan(trans.GetZ()) ||
                    std::abs(trans.GetX()) > 1000000 || std::abs(trans.GetY()) > 1000000 || std::abs(trans.GetZ()) > 1000000)
                {
                    Log(Debug::Error) << "Invalid pose for joint " << i << " ("
                                      << mAnimationSkeleton->GetJoint(i).mName << "): "
                                      << trans.GetX() << ", " << trans.GetY() << ", " << trans.GetZ();
                }
            }
            // Also log ragdoll input pose and root offset
            Log(Debug::Info) << "Ragdoll rootOffset (world pos): "
                             << rootOffset.GetX() << ", "
                             << rootOffset.GetY() << ", "
                             << rootOffset.GetZ()
                             << " | joint[0] model-space trans: "
                             << mRagdollPoseBuffer[0].GetTranslation().GetX() << ", "
                             << mRagdollPoseBuffer[0].GetTranslation().GetY() << ", "
                             << mRagdollPoseBuffer[0].GetTranslation().GetZ()
                             << " | joint[1] model-space trans: "
                             << (mRagdollPoseBuffer.size() > 1 ? mRagdollPoseBuffer[1].GetTranslation().GetX() : 0) << ", "
                             << (mRagdollPoseBuffer.size() > 1 ? mRagdollPoseBuffer[1].GetTranslation().GetY() : 0) << ", "
                             << (mRagdollPoseBuffer.size() > 1 ? mRagdollPoseBuffer[1].GetTranslation().GetZ() : 0);

            // Log output from mapper for pelvis (root bone in animation skeleton)
            if (mRootOsgBoneIndex < mOsgBones.size())
            {
                int rootAnimIdx = mOsgToAnimIndex[mRootOsgBoneIndex];
                if (rootAnimIdx >= 0 && rootAnimIdx < static_cast<int>(mAnimationPoseBuffer.size()))
                {
                    JPH::Vec3 animTrans = mAnimationPoseBuffer[rootAnimIdx].GetTranslation();
                    JPH::Quat animRot = mAnimationPoseBuffer[rootAnimIdx].GetRotation().GetQuaternion();
                    Log(Debug::Info) << "Mapper output for pelvis (animIdx=" << rootAnimIdx << "): trans=("
                                     << animTrans.GetX() << ", " << animTrans.GetY() << ", " << animTrans.GetZ()
                                     << ") rot=(" << animRot.GetX() << ", " << animRot.GetY() << ", "
                                     << animRot.GetZ() << ", " << animRot.GetW() << ")";
                }
            }
        }

        // Step 4: Handle the non-physics ancestor bones (e.g., Bip01)
        // These bones are not part of the physics skeleton, but we need to position them
        // so the physics root (pelvis) ends up at the correct world position.
        //
        // IMPORTANT: We use STORED bind pose values to avoid feedback loops.
        // Only translation is updated - rotation stays at original bind pose value.
        //
        // We also store the computed Bip01 world matrix for use in Step 6, because
        // OSG's cached world matrices won't be updated until the next traversal.
        osg::Matrix computedBip01World;  // Will be used in Step 6 for pelvis parent
        bool haveBip01World = false;

        if (mRootOsgBoneIndex < mOsgBones.size() && mRootRagdollJointIndex >= 0)
        {
            OsgBoneInfo& rootBone = mOsgBones[mRootOsgBoneIndex];

            // Get the physics root's world position
            osg::Vec3f physicsRootWorldPos = rootWorldPos;  // From GetPose() rootOffset

            // Find Bip01 (the OSG parent of the physics root / pelvis)
            if (rootBone.node->getNumParents() > 0)
            {
                osg::Node* osgParent = rootBone.node->getParent(0);
                auto* bip01 = dynamic_cast<osg::MatrixTransform*>(osgParent);
                if (bip01)
                {
                    // Use STORED bind pose rotation (not current, which may have drifted)
                    osg::Quat bip01LocalRot = mBip01BindPoseLocal.getRotate();

                    // Get Bip01's parent world transform
                    osg::Matrix bip01ParentWorld;
                    if (bip01->getNumParents() > 0)
                    {
                        osg::NodePathList nodePaths = bip01->getParent(0)->getParentalNodePaths();
                        if (!nodePaths.empty())
                        {
                            osg::NodePath pathWithParent = nodePaths[0];
                            pathWithParent.push_back(bip01->getParent(0));
                            bip01ParentWorld = osg::computeLocalToWorld(pathWithParent);
                        }
                    }

                    // Use STORED pelvis offset (not current, which may have been modified)
                    osg::Vec3f pelvisLocalPos = mPelvisBindPoseLocalPos;

                    // Bip01's world rotation (using stored bind pose local rotation)
                    osg::Quat bip01ParentRot = bip01ParentWorld.getRotate();
                    osg::Quat bip01WorldRot = bip01LocalRot * bip01ParentRot;

                    // Calculate where Bip01 should be so pelvis ends up at physicsRootWorldPos
                    // pelvisWorld = pelvisLocal * bip01World
                    // physicsRootWorldPos = pelvisLocalPos * bip01WorldRot + bip01WorldPos
                    // bip01WorldPos = physicsRootWorldPos - pelvisLocalPos * bip01WorldRot
                    osg::Matrix bip01WorldRotMat;
                    bip01WorldRotMat.makeRotate(bip01WorldRot);
                    osg::Vec3f rotatedPelvisOffset = pelvisLocalPos * bip01WorldRotMat;
                    osg::Vec3f bip01WorldPos = physicsRootWorldPos - rotatedPelvisOffset;

                    // Convert Bip01 world position to local (relative to its parent)
                    osg::Vec3f bip01ParentPos = bip01ParentWorld.getTrans();
                    osg::Vec3f bip01DeltaPos = bip01WorldPos - bip01ParentPos;
                    osg::Matrix invBip01ParentRot;
                    invBip01ParentRot.makeRotate(bip01ParentRot.inverse());
                    osg::Vec3f bip01LocalPos = bip01DeltaPos * invBip01ParentRot;

                    // Apply stored rotation and new translation
                    auto* nifBip01 = dynamic_cast<NifOsg::MatrixTransform*>(bip01);
                    if (nifBip01)
                    {
                        nifBip01->setRotation(bip01LocalRot);  // Stored bind pose rotation
                        nifBip01->setTranslation(bip01LocalPos);  // New position
                    }
                    else
                    {
                        osg::Matrix bip01LocalMatrix;
                        bip01LocalMatrix.makeRotate(bip01LocalRot);  // Stored bind pose rotation
                        bip01LocalMatrix.setTrans(bip01LocalPos);    // New position
                        bip01->setMatrix(bip01LocalMatrix);
                    }

                    // Store the computed Bip01 world matrix for use in Step 6
                    // This is the world matrix that Bip01 will have AFTER OSG processes our update
                    computedBip01World.makeRotate(bip01WorldRot);
                    computedBip01World.setTrans(bip01WorldPos);
                    haveBip01World = true;

                    // Debug: log what we applied
                    static int bip01DebugCount = 0;
                    if (bip01DebugCount++ < 10)
                    {
                        Log(Debug::Info) << "Applied to Bip01: localPos=("
                                         << bip01LocalPos.x() << ", " << bip01LocalPos.y() << ", " << bip01LocalPos.z()
                                         << ") computedWorldPos=(" << bip01WorldPos.x() << ", " << bip01WorldPos.y() << ", " << bip01WorldPos.z()
                                         << ") physicsRootWorldPos=(" << physicsRootWorldPos.x() << ", " << physicsRootWorldPos.y() << ", " << physicsRootWorldPos.z()
                                         << ") pelvisBindOffset=(" << pelvisLocalPos.x() << ", " << pelvisLocalPos.y() << ", " << pelvisLocalPos.z() << ")";
                    }
                }
            }
        }

        // Step 5: Build world matrices for all animation joints
        std::vector<osg::Matrix> worldMatrices(mAnimationSkeleton->GetJointCount());

        for (int animIdx = 0; animIdx < mAnimationSkeleton->GetJointCount(); ++animIdx)
        {
            const JPH::Mat44& modelSpace = mAnimationPoseBuffer[animIdx];

            // Convert model space to world space by adding root offset to translation
            osg::Matrix modelOsg = toOsgMatrix(modelSpace);
            osg::Vec3f modelTrans = modelOsg.getTrans();
            osg::Vec3f worldTrans = modelTrans + rootWorldPos;

            // Build world matrix
            osg::Matrix worldMat;
            worldMat.makeRotate(modelOsg.getRotate());
            worldMat.setTrans(worldTrans);
            worldMatrices[animIdx] = worldMat;
        }

        // CRITICAL FIX: Override Bip01's world matrix with the one we computed in Step 4.
        // The SkeletonMapper doesn't handle Bip01 (it's not a physics bone), so
        // mAnimationPoseBuffer[bip01AnimIdx] contains stale/wrong data. We need to use
        // the correct Bip01 world transform that we computed based on physics root position.
        if (haveBip01World && mBip01OsgIndex >= 0)
        {
            int bip01AnimIdx = mOsgToAnimIndex[mBip01OsgIndex];
            if (bip01AnimIdx >= 0 && bip01AnimIdx < static_cast<int>(worldMatrices.size()))
            {
                worldMatrices[bip01AnimIdx] = computedBip01World;

                static int bip01OverrideLogCount = 0;
                if (bip01OverrideLogCount++ < 5)
                {
                    Log(Debug::Info) << "Step 5: Overrode Bip01 worldMatrix[" << bip01AnimIdx << "] with computedBip01World: ("
                                     << computedBip01World.getTrans().x() << ", "
                                     << computedBip01World.getTrans().y() << ", "
                                     << computedBip01World.getTrans().z() << ")";
                }
            }
        }

        // Step 6: Apply to OSG bones, computing local transforms from world matrices
        for (int animIdx = 0; animIdx < mAnimationSkeleton->GetJointCount(); ++animIdx)
        {
            auto it = mAnimToOsgIndex.find(animIdx);
            if (it == mAnimToOsgIndex.end())
                continue;

            size_t osgIdx = it->second;
            OsgBoneInfo& info = mOsgBones[osgIdx];

            // Skip bones with no parent in our skeleton - they need special handling
            // (e.g., Bip01 which is already handled in Step 4, or mesh nodes)
            // Applying world coordinates as local transforms would break rendering
            if (info.osgParentIndex < 0)
            {
                static int skippedBoneLogCount = 0;
                if (skippedBoneLogCount++ < 20)
                {
                    Log(Debug::Info) << "SKIPPED bone with no OSG parent: " << info.name
                                     << " (animIdx=" << animIdx << ", joltMapped=" << info.joltMappedIndex << ")";
                }
                continue;
            }

            const osg::Matrix& worldMat = worldMatrices[animIdx];

            // Compute local transform relative to parent
            // Note: We already skipped bones with osgParentIndex < 0 above
            osg::Matrix localMat;
            int parentAnimIdx = mOsgToAnimIndex[info.osgParentIndex];
            if (parentAnimIdx >= 0 && parentAnimIdx < static_cast<int>(worldMatrices.size()))
            {
                // Parent is in our animation skeleton - use our computed world matrix
                osg::Matrix parentInv;
                parentInv.invert(worldMatrices[parentAnimIdx]);
                localMat = worldMat * parentInv;
            }
            else
            {
                // Parent is not in animation skeleton (e.g., Bip01 is parent of Pelvis)
                // ISSUE: getWorldMatrix() queries OSG's cached matrix, but we may have just
                // updated Bip01's local matrix in Step 4. OSG won't propagate this to world
                // matrix until the next traversal. We need to use the computed world matrix.
                osg::Matrix parentWorld;

                // Check if the parent is Bip01 (which we updated in Step 4)
                if (haveBip01World && mBip01OsgIndex >= 0 && info.osgParentIndex == mBip01OsgIndex)
                {
                    // Use the Bip01 world matrix we computed in Step 4
                    parentWorld = computedBip01World;
                }
                else
                {
                    // Fallback to OSG's cached world matrix
                    parentWorld = getWorldMatrix(mOsgBones[info.osgParentIndex].node);
                }

                // Debug: Log pelvis specifically
                static int pelvisDebugCount = 0;
                std::string lowerName = Misc::StringUtils::lowerCase(info.name);
                bool isPelvis = (lowerName.find("pelvis") != std::string::npos);

                if (isPelvis && pelvisDebugCount < 10)
                {
                    Log(Debug::Info) << "PELVIS: targetWorld=("
                                     << worldMat.getTrans().x() << ", " << worldMat.getTrans().y() << ", " << worldMat.getTrans().z()
                                     << ") parentWorld=(" << parentWorld.getTrans().x() << ", " << parentWorld.getTrans().y() << ", " << parentWorld.getTrans().z()
                                     << ") parent=" << mOsgBones[info.osgParentIndex].name
                                     << " usedComputedBip01=" << (haveBip01World && mBip01OsgIndex >= 0 && info.osgParentIndex == mBip01OsgIndex);
                }

                osg::Matrix parentInv;
                parentInv.invert(parentWorld);
                localMat = worldMat * parentInv;

                if (isPelvis && pelvisDebugCount++ < 10)
                {
                    Log(Debug::Info) << "PELVIS: computedLocal=("
                                     << localMat.getTrans().x() << ", " << localMat.getTrans().y() << ", " << localMat.getTrans().z() << ")";
                }
            }

            // Debug: Check for extreme values that might make mesh disappear
            static int boneDebugCount = 0;
            osg::Vec3f localPos = localMat.getTrans();
            if (boneDebugCount < 3 && animIdx < 5)  // Log first few bones for first few frames
            {
                Log(Debug::Info) << "Bone " << info.name << " localPos=("
                                 << localPos.x() << ", " << localPos.y() << ", " << localPos.z()
                                 << ") worldPos=(" << worldMat.getTrans().x() << ", "
                                 << worldMat.getTrans().y() << ", " << worldMat.getTrans().z() << ")";
            }
            if (animIdx == static_cast<int>(mAnimationSkeleton->GetJointCount()) - 1)
                boneDebugCount++;

            // Check for NaN or extreme values
            if (std::isnan(localPos.x()) || std::isnan(localPos.y()) || std::isnan(localPos.z()) ||
                std::abs(localPos.x()) > 100000 || std::abs(localPos.y()) > 100000 || std::abs(localPos.z()) > 100000)
            {
                Log(Debug::Error) << "EXTREME local transform for " << info.name << ": ("
                                  << localPos.x() << ", " << localPos.y() << ", " << localPos.z() << ")";
                continue;  // Skip applying this transform
            }

            // Apply to OSG node
            auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(info.node);
            if (nifTransform)
            {
                nifTransform->setRotation(localMat.getRotate());
                nifTransform->setTranslation(localMat.getTrans());
            }
            else
            {
                info.node->setMatrix(localMat);
            }
        }
    }

    void RagdollSkeletonMapper::applyModelSpaceTransform(size_t boneIndex, const JPH::Mat44& modelSpaceTransform)
    {
        OsgBoneInfo& info = mOsgBones[boneIndex];

        // Get parent's model-space transform (using animation skeleton indexing)
        JPH::Mat44 parentModelSpace = JPH::Mat44::sIdentity();
        if (info.osgParentIndex >= 0)
        {
            int parentAnimIdx = mOsgToAnimIndex[info.osgParentIndex];
            if (parentAnimIdx >= 0 && parentAnimIdx < static_cast<int>(mAnimationPoseBuffer.size()))
            {
                parentModelSpace = mAnimationPoseBuffer[parentAnimIdx];
            }
        }

        // Compute local transform: local = inverse(parent) * modelSpace
        JPH::Mat44 localTransform = parentModelSpace.Inversed() * modelSpaceTransform;
        osg::Matrix localOsg = toOsgMatrix(localTransform);

        // Apply to OSG node
        auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(info.node);
        if (nifTransform)
        {
            nifTransform->setRotation(localOsg.getRotate());
            nifTransform->setTranslation(localOsg.getTrans());
        }
        else
        {
            info.node->setMatrix(localOsg);
        }
    }

    void RagdollSkeletonMapper::mapOsgToRagdoll(float blendWeight)
    {
        if (!mIsValid || blendWeight <= 0.0f)
            return;

        JPH::BodyInterface& bodyInterface = mPhysicsSystem->GetBodyInterface();

        // Step 1: Get current OSG bone poses in model space (using animation skeleton indexing)
        for (size_t osgIdx = 0; osgIdx < mOsgBones.size(); ++osgIdx)
        {
            int animIdx = mOsgToAnimIndex[osgIdx];
            if (animIdx >= 0 && animIdx < static_cast<int>(mAnimationPoseBuffer.size()))
            {
                osg::Matrix worldMat = getWorldMatrix(mOsgBones[osgIdx].node);
                osg::Matrix modelMat = worldMat * mSkeletonRootInverse;
                mAnimationPoseBuffer[animIdx] = toJoltMat44(modelMat);
            }
        }

        // Step 2: Map animation -> ragdoll using reverse mapping
        mMapper.MapReverse(
            mAnimationPoseBuffer.data(),
            mRagdollPoseBuffer.data()
        );

        // Step 3: Apply to ragdoll bodies (with optional blending)
        for (int j = 0; j < mRagdollSkeleton->GetJointCount(); ++j)
        {
            JPH::BodyID bodyId = mRagdoll->GetBodyID(j);
            if (bodyId.IsInvalid())
                continue;

            // Convert model space to world space
            osg::Matrix modelOsg = toOsgMatrix(mRagdollPoseBuffer[j]);
            osg::Matrix worldOsg = modelOsg * mSkeletonRootTransform;

            osg::Vec3f targetPos = worldOsg.getTrans();
            osg::Quat targetRot = worldOsg.getRotate();

            if (blendWeight >= 1.0f)
            {
                // Full animation control
                bodyInterface.SetPositionAndRotation(
                    bodyId,
                    Misc::Convert::toJolt<JPH::RVec3>(targetPos),
                    Misc::Convert::toJolt(targetRot),
                    JPH::EActivation::Activate
                );
            }
            else
            {
                // Blend between physics and animation
                JPH::RVec3 currentPos;
                JPH::Quat currentRot;
                bodyInterface.GetPositionAndRotation(bodyId, currentPos, currentRot);

                osg::Vec3f blendedPos = Misc::Convert::toOsg(currentPos) * (1.0f - blendWeight)
                                       + targetPos * blendWeight;

                osg::Quat blendedRot;
                blendedRot.slerp(blendWeight, Misc::Convert::toOsg(currentRot), targetRot);

                bodyInterface.SetPositionAndRotation(
                    bodyId,
                    Misc::Convert::toJolt<JPH::RVec3>(blendedPos),
                    Misc::Convert::toJolt(blendedRot),
                    JPH::EActivation::Activate
                );
            }
        }
    }

    osg::Vec3f RagdollSkeletonMapper::getRootPosition() const
    {
        if (!mIsValid || !mRagdoll)
            return osg::Vec3f();

        JPH::RVec3 rootPos;
        JPH::Quat rootRot;
        mRagdoll->GetRootTransform(rootPos, rootRot);
        return Misc::Convert::toOsg(rootPos);
    }

    osg::Quat RagdollSkeletonMapper::getRootRotation() const
    {
        if (!mIsValid || !mRagdoll)
            return osg::Quat();

        JPH::RVec3 rootPos;
        JPH::Quat rootRot;
        mRagdoll->GetRootTransform(rootPos, rootRot);
        return Misc::Convert::toOsg(rootRot);
    }

}  // namespace MWPhysics
