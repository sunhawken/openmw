#include "havokragdolltemplate.hpp"

#include <cmath>
#include <string>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/physics.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

namespace MWPhysics
{
    namespace
    {
        // Bethesda Havok physics data (Oblivion onward) is authored in Havok's
        // native meter-based units and needs converting to the render-geometry
        // unit scale used by the rest of the NIF (and by this fork's own bone
        // geometry, which is derived from that same render-geometry scale).
        // 69.9915 is the standard conversion factor used across NIF/Havok
        // modding tools (NifSkope, HavokMax, etc.) for Bethesda content.
        constexpr float kHavokToNifScale = 69.9915f;

        const Nif::bhkRagdollTemplate* findRagdollTemplate(const Nif::NiAVObject& node)
        {
            for (const auto& extra : node.getExtraList())
            {
                if (!extra.empty() && extra->mRecordType == Nif::RC_bhkRagdollTemplate)
                    return static_cast<const Nif::bhkRagdollTemplate*>(extra.getPtr());
            }

            if (const auto* niNode = dynamic_cast<const Nif::NiNode*>(&node))
            {
                for (const auto& child : niNode->mChildren)
                {
                    if (child.empty())
                        continue;
                    if (const Nif::bhkRagdollTemplate* found = findRagdollTemplate(*child.getPtr()))
                        return found;
                }
            }

            return nullptr;
        }

        // Overlays joint limits onto a config that's already been seeded with the
        // heuristic defaults, using this bone's own authored constraint data —
        // but only for constraint types with cone/twist semantics compatible with
        // Jolt's SwingTwistConstraint (which is what createConstraintSettings()
        // always builds). Other constraint types keep the heuristic limits.
        void applyConstraint(const Nif::bhkWrappedConstraintData& constraint, JointConfig& config)
        {
            switch (constraint.mType)
            {
                case Nif::HkConstraintType::Ragdoll:
                {
                    const Nif::bhkRagdollConstraintCInfo& info = constraint.mRagdollInfo;
                    config.swingLimit = info.mConeMaxAngle;
                    config.planeSwingLimit = std::max(std::abs(info.mPlaneMinAngle), std::abs(info.mPlaneMaxAngle));
                    config.twistMinLimit = info.mTwistMinAngle;
                    config.twistMaxLimit = info.mTwistMaxAngle;
                    break;
                }
                case Nif::HkConstraintType::LimitedHinge:
                {
                    const Nif::bhkLimitedHingeConstraintCInfo& info = constraint.mLimitedHingeInfo;
                    // A hinge only rotates around one axis; approximate it as a very
                    // tight swing cone with the hinge's own range used as the twist limit.
                    config.swingLimit = 0.05f;
                    config.planeSwingLimit = 0.05f;
                    config.twistMinLimit = info.mMinAngle;
                    config.twistMaxLimit = info.mMaxAngle;
                    break;
                }
                default:
                    // BallAndSocket / unlimited Hinge / StiffSpring / Prismatic / Malleable
                    // don't have angle-limit data that maps onto a cone-twist constraint -
                    // leave the heuristic default limits in place.
                    break;
            }
        }
    }

    RagdollSettingsBuilder::JointConfigMap loadHavokRagdollTemplate(
        const MWWorld::Ptr& ptr, Resource::ResourceSystem* resourceSystem)
    {
        RagdollSettingsBuilder::JointConfigMap result;
        if (!resourceSystem)
            return result;

        const VFS::Path::Normalized model = ptr.getClass().getCorrectedModel(ptr);
        if (model.empty())
            return result;

        Nif::NIFFilePtr nifFile;
        try
        {
            nifFile = resourceSystem->getNifFileManager()->get(model);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Verbose) << "loadHavokRagdollTemplate: failed to load " << model.value() << ": " << e.what();
            return result;
        }

        if (!nifFile)
            return result;

        const Nif::FileView file(*nifFile);

        const Nif::bhkRagdollTemplate* ragdollTemplate = nullptr;
        for (std::size_t i = 0; i < file.numRoots() && !ragdollTemplate; ++i)
        {
            if (const auto* node = dynamic_cast<const Nif::NiAVObject*>(file.getRoot(i)))
                ragdollTemplate = findRagdollTemplate(*node);
        }

        if (!ragdollTemplate)
            return result;

        for (const auto& bonePtr : ragdollTemplate->mBones)
        {
            if (bonePtr.empty())
                continue;
            const Nif::bhkRagdollTemplateData& bone = *bonePtr.getPtr();
            if (bone.mName.empty())
                continue;

            const std::string boneName = Misc::StringUtils::lowerCase(bone.mName);

            JointConfig config = RagdollSettingsBuilder::getDefaultConfig(boneName);
            config.mass = bone.mMass;
            config.friction = bone.mFriction;
            config.restitution = bone.mRestitution;
            config.explicitRadius = bone.mRadius * kHavokToNifScale;

            if (!bone.mConstraints.empty())
                applyConstraint(bone.mConstraints.front(), config);

            result[boneName] = config;
        }

        if (!result.empty())
            Log(Debug::Info) << "loadHavokRagdollTemplate: found authored ragdoll data for " << result.size()
                              << " bones in " << model.value();

        return result;
    }
}
