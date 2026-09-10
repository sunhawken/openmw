#include "jiggleautorig.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/Group>
#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/ValueObject>

#include <components/debug/debuglog.hpp>
#include <components/misc/jigglezoffset.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/sceneutil/skeleton.hpp>
#include <components/settings/values.hpp>

#include "jigglebonecontroller.hpp"

namespace MWRender
{
    namespace
    {
        using Rig = SceneUtil::RigGeometry;

        constexpr std::array<std::string_view, 4> sJiggleBones
            = { "bip01 l breast", "bip01 r breast", "bip01 l butt", "bip01 r butt" };

        // Marks a bone node we injected (an identity child of its parent), so on a resync we can
        // tell it apart from an externally-rigged (.bat) jiggle bone that has its own bind offset.
        constexpr const char* sAutoRigMarker = "openmw_autorig";

        // Mirrors autorig.py BREAST_CONFIG / BUTT_CONFIG (anchor band) + weight paint params.
        struct Config
        {
            std::string_view mLandmark; // bone whose mesh-space Z anchors the candidate band
            std::string_view mParent; // existing weighted bone the jiggle bone hangs from
            float mZLo, mZHi;
            bool mMaxX; // most-forward (breast) vs most-rearward (butt)
            float mMinAbsY;
            int mTopK;
            float mRadius, mZRadius, mMaxWeight; // cone weight paint
        };
        constexpr Config sBreast{ "bip01 spine2", "bip01 spine2", -10.f, 14.f, true, 0.5f, 12, 9.f, 6.f, 0.25f };
        constexpr Config sButt{ "bip01 pelvis", "bip01 pelvis", -7.f, 9.f, false, 0.5f, 12, 9.f, 6.f, 0.25f };

        struct Target
        {
            std::string_view mBoneLower; // RigGeometry bone name (lowercase, resolved via getBone)
            std::string_view mBoneNode; // scene-graph node name (original case; controller checks Breast/Butt)
            bool mLeft;
            const Config& mConfig;
        };
        const std::array<Target, 4> sTargets = { {
            { "bip01 l breast", "Bip01 L Breast", true, sBreast },
            { "bip01 r breast", "Bip01 R Breast", false, sBreast },
            { "bip01 l butt", "Bip01 L Butt", true, sButt },
            { "bip01 r butt", "Bip01 R Butt", false, sButt },
        } };

        class RigCollector : public osg::NodeVisitor
        {
        public:
            RigCollector()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            void apply(osg::Drawable& d) override
            {
                if (auto* rig = dynamic_cast<Rig*>(&d))
                    mRigs.push_back(rig);
            }
            std::vector<Rig*> mRigs;
        };

        class SkeletonFinder : public osg::NodeVisitor
        {
        public:
            SkeletonFinder()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }
            void apply(osg::Group& group) override
            {
                if (mSkeleton)
                    return;
                if (auto* skel = dynamic_cast<SceneUtil::Skeleton*>(&group))
                {
                    mSkeleton = skel;
                    return;
                }
                traverse(group);
            }
            SceneUtil::Skeleton* mSkeleton = nullptr;
        };

        const osg::Vec3Array* sourceVerts(const Rig& rig)
        {
            osg::ref_ptr<osg::Geometry> src = rig.getSourceGeometry();
            if (!src)
                return nullptr;
            return dynamic_cast<const osg::Vec3Array*>(src->getVertexArray());
        }

        int boneIndex(const std::vector<std::string>& names, std::string_view bone)
        {
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == bone)
                    return static_cast<int>(i);
            return -1;
        }

        std::optional<float> landmarkZ(const std::vector<Rig*>& rigs, std::string_view landmark)
        {
            double sum = 0.0;
            std::size_t count = 0;
            for (Rig* rig : rigs)
            {
                const int bi = boneIndex(rig->getInfluenceBoneNames(), landmark);
                if (bi < 0)
                    continue;
                const osg::Vec3Array* verts = sourceVerts(*rig);
                if (!verts)
                    continue;
                const std::vector<Rig::BoneWeights> perVertex = rig->getPerVertexInfluences(verts->size());
                for (std::size_t v = 0; v < perVertex.size(); ++v)
                    for (const auto& [bone, weight] : perVertex[v])
                        if (static_cast<int>(bone) == bi && weight > 0.f)
                        {
                            sum += (*verts)[v].z();
                            ++count;
                            break;
                        }
            }
            if (!count)
                return std::nullopt;
            return static_cast<float>(sum / count);
        }

        std::optional<osg::Vec3f> findAnchor(const std::vector<Rig*>& rigs, const Config& cfg, bool left, float lz)
        {
            const float zMin = lz + cfg.mZLo;
            const float zMax = lz + cfg.mZHi;
            std::vector<osg::Vec3f> cand;
            for (Rig* rig : rigs)
            {
                const osg::Vec3Array* verts = sourceVerts(*rig);
                if (!verts)
                    continue;
                for (const osg::Vec3f& p : *verts)
                {
                    if (p.z() < zMin || p.z() > zMax)
                        continue;
                    if (left && p.y() <= cfg.mMinAbsY)
                        continue;
                    if (!left && p.y() >= -cfg.mMinAbsY)
                        continue;
                    cand.push_back(p);
                }
            }
            if (cand.empty())
                return std::nullopt;
            std::sort(cand.begin(), cand.end(),
                [&](const osg::Vec3f& a, const osg::Vec3f& b) { return cfg.mMaxX ? a.x() > b.x() : a.x() < b.x(); });
            const std::size_t k = std::min<std::size_t>(cfg.mTopK, cand.size());
            osg::Vec3f sum(0, 0, 0);
            for (std::size_t i = 0; i < k; ++i)
                sum += cand[i];
            return sum / static_cast<float>(k);
        }

        // autorig.py Pass 1 cone: sqrt falloff within radius, vertical reach capped at z_radius,
        // same-side guard, returns vertex index -> weight.
        // Walk up from a rig to the nearest ancestor tagged (by the NIF loader) with its source
        // mesh file, so a rig can be matched against the auto-rig blacklist.
        std::string meshFileFor(osg::Node* node)
        {
            while (node)
            {
                std::string file;
                if (node->getUserValue("meshFileName", file) && !file.empty())
                    return file;
                node = node->getNumParents() > 0 ? node->getParent(0) : nullptr;
            }
            return {};
        }

        bool meshBlacklisted(const std::string& meshFile)
        {
            if (meshFile.empty())
                return false;
            for (const std::string& pattern : Settings::game().mJiggleAutoRigBlacklist.get())
            {
                if (!pattern.empty() && meshFile.find(Misc::StringUtils::lowerCase(pattern)) != std::string::npos)
                    return true;
            }
            return false;
        }

        std::unordered_map<std::size_t, float> coneWeights(
            const osg::Vec3Array& verts, const osg::Vec3f& anchor, const Config& cfg, bool left)
        {
            std::unordered_map<std::size_t, float> weights;
            for (std::size_t i = 0; i < verts.size(); ++i)
            {
                const osg::Vec3f& p = verts[i];
                if (left && p.y() <= cfg.mMinAbsY)
                    continue;
                if (!left && p.y() >= -cfg.mMinAbsY)
                    continue;
                if (std::abs(p.z() - anchor.z()) > cfg.mZRadius)
                    continue;
                const float dist = (p - anchor).length();
                if (dist >= cfg.mRadius)
                    continue;
                const float n = dist / cfg.mRadius;
                const float w = cfg.mMaxWeight * std::sqrt(std::max(0.f, 1.f - n * n));
                if (w > 0.01f)
                    weights[i] = w;
            }
            return weights;
        }
    }

    void JiggleAutoRig::run(osg::Group* objectRoot, bool isPlayer)
    {
        if (!objectRoot || !Settings::game().mJiggleAutoRig)
            return;
        const bool debug = Settings::game().mJiggleAutoRigDebug;

        RigCollector rc;
        objectRoot->accept(rc);
        if (rc.mRigs.empty())
            return;

        // Drop blacklisted meshes up front so they take part in neither anchor detection nor
        // painting (an odd armor can't get bad jiggle nor drag the shared body anchor off).
        std::vector<Rig*> rigs;
        rigs.reserve(rc.mRigs.size());
        std::string bodyMeshFile; // the chest/body part (weights spine2) - keys per-mesh Z offsets
        for (Rig* rig : rc.mRigs)
        {
            osg::Node* start = rig->getNumParents() > 0 ? rig->getParent(0) : nullptr;
            const std::string meshFile = meshFileFor(start);
            if (meshBlacklisted(meshFile))
            {
                if (debug)
                    Log(Debug::Warning) << "Jiggle auto-rig: skipping blacklisted mesh " << meshFile;
                continue;
            }
            if (debug)
                Log(Debug::Warning) << "Jiggle auto-rig: considering mesh " << meshFile;
            if (bodyMeshFile.empty() && !meshFile.empty()
                && boneIndex(rig->getInfluenceBoneNames(), sBreast.mParent) >= 0)
                bodyMeshFile = meshFile;
            rigs.push_back(rig);
        }
        if (rigs.empty())
            return;

        // For the player, snap the breast/butt Z-offset sliders to whatever chest mesh is currently
        // worn (naked body, clothing or armor), so each outfit keeps its own tuning and changing
        // equipment automatically applies that outfit's saved offset - no manual re-adjusting. If an
        // outfit has no saved value yet it snaps to 0 (a clean default) rather than leaving the
        // previous outfit's offset in place. The settings window writes slider changes back keyed to
        // this same mesh (see mwgui/settingswindow.cpp).
        if (isPlayer && !bodyMeshFile.empty())
        {
            Misc::JiggleZOffset::currentPlayerMesh() = bodyMeshFile;
            const auto stored = Misc::JiggleZOffset::lookup(bodyMeshFile);
            const float breast = stored ? stored->first : 0.f;
            const float butt = stored ? stored->second : 0.f;
            Settings::game().mJiggleBoneBreastZOffset.set(breast);
            Settings::game().mJiggleBoneButtZOffset.set(butt);
            if (debug)
                Log(Debug::Warning) << "Jiggle auto-rig: snapped Z offsets to " << bodyMeshFile << " breast=" << breast
                                    << " butt=" << butt << (stored ? " (saved)" : " (default)");
        }

        SkeletonFinder sf;
        objectRoot->accept(sf);
        if (!sf.mSkeleton)
            return;
        SceneUtil::Skeleton* skeleton = sf.mSkeleton;

        // Detect only the jiggle bones WE injected on an earlier pass (marked identity children of
        // the parent bone) so a resync doesn't recreate them. Pre-existing skeleton jiggle bone
        // nodes - e.g. from a jiggle-bones-skeleton mod - are not ours; we ignore them and drive the
        // body with our own identity-child bones, whose bind pose we control. A mesh that already
        // carries jiggle WEIGHTS (rigged by the .bat) is skipped per-mesh further below.
        std::array<bool, 4> haveOurBone = { false, false, false, false };
        for (std::size_t t = 0; t < sTargets.size(); ++t)
        {
            SceneUtil::Bone* parent = skeleton->getBone(std::string(sTargets[t].mConfig.mParent));
            osg::Group* parentNode = parent ? parent->mNode.get() : nullptr;
            if (!parentNode)
                continue;
            for (unsigned int c = 0; c < parentNode->getNumChildren(); ++c)
            {
                osg::Node* child = parentNode->getChild(c);
                if (!Misc::StringUtils::ciEqual(child->getName(), std::string(sTargets[t].mBoneNode)))
                    continue;
                bool ours = false;
                child->getUserValue(sAutoRigMarker, ours);
                if (ours)
                {
                    haveOurBone[t] = true;
                    break;
                }
            }
        }

        // Anchors are recomputed each pass from whatever meshes are currently attached, so a piece
        // equipped later (e.g. armor covering the chest) is painted from its own geometry.
        std::array<std::optional<osg::Vec3f>, 4> anchors;
        for (std::size_t t = 0; t < sTargets.size(); ++t)
        {
            const auto lz = landmarkZ(rigs, sTargets[t].mConfig.mLandmark);
            if (lz)
                anchors[t] = findAnchor(rigs, sTargets[t].mConfig, sTargets[t].mLeft, *lz);
        }

        // 1) Create any jiggle bone nodes that don't exist yet (identity children of their weighted
        //    parent bone) and attach the spring controller. Idempotent across equip/unequip resyncs.
        bool addedAny = false;
        for (std::size_t t = 0; t < sTargets.size(); ++t)
        {
            if (haveOurBone[t] || !anchors[t])
                continue;
            const Target& tgt = sTargets[t];

            SceneUtil::Bone* parent = skeleton->getBone(std::string(tgt.mConfig.mParent));
            osg::MatrixTransform* parentNode = parent ? parent->mNode.get() : nullptr;
            if (!parentNode)
            {
                if (debug)
                    Log(Debug::Warning) << "Jiggle auto-rig: no parent bone " << tgt.mConfig.mParent << " for "
                                        << tgt.mBoneNode;
                anchors[t].reset();
                continue;
            }

            osg::ref_ptr<osg::MatrixTransform> boneNode = new osg::MatrixTransform(osg::Matrix::identity());
            boneNode->setName(std::string(tgt.mBoneNode));
            boneNode->setDataVariance(osg::Object::DYNAMIC);
            boneNode->setUserValue(sAutoRigMarker, true);
            parentNode->addChild(boneNode);
            boneNode->addUpdateCallback(new JiggleBoneController(debug));
            haveOurBone[t] = true;
            addedAny = true;
            if (debug)
                Log(Debug::Warning) << "Jiggle auto-rig: added bone " << tgt.mBoneNode << " under "
                                    << tgt.mConfig.mParent << " anchor " << anchors[t]->x() << "," << anchors[t]->y()
                                    << "," << anchors[t]->z();
        }

        // Rebuild the bone cache/hierarchy so newly added bones resolve by name. (Adding a child deep
        // in the bone tree doesn't trigger Skeleton::childInserted, so invalidate the cache manually.)
        if (addedAny)
            skeleton->markDirty();

        // 2) Paint weights: for each attached mesh that doesn't already carry jiggle bones and weights
        //    a target's parent, add the jiggle bone (reusing that mesh's parent inverse-bind so the
        //    rest pose is exact) + cone weights. Meshes already carrying jiggle bones (rigged by the
        //    .bat, or painted on an earlier pass) are skipped, so a resync only touches new parts.
        //    Blacklisted meshes were already filtered out of `rigs`, so they get no AUTO rig here -
        //    but their load-time seam fix, .bat-rigged bones, controllers and z-offset still apply.
        int paintedMeshes = 0;
        for (Rig* rig : rigs)
        {
            const osg::Vec3Array* verts = sourceVerts(*rig);
            if (!verts || verts->empty())
                continue;
            std::vector<std::string> names = rig->getInfluenceBoneNames();

            bool alreadyRigged = false;
            for (const std::string& name : names)
                for (std::string_view jb : sJiggleBones)
                    if (name == jb)
                        alreadyRigged = true;
            if (alreadyRigged)
                continue;

            std::vector<Rig::BoneInfo> bones = rig->getBoneInfoList();
            std::vector<Rig::BoneWeights> perVertex = rig->getPerVertexInfluences(verts->size());

            bool rigChanged = false;
            for (std::size_t t = 0; t < sTargets.size(); ++t)
            {
                if (!haveOurBone[t] || !anchors[t])
                    continue;
                const Target& tgt = sTargets[t];
                const int parentIdx = boneIndex(names, tgt.mConfig.mParent);
                if (parentIdx < 0)
                    continue; // this mesh isn't part of the region the bone hangs from

                auto weights = coneWeights(*verts, *anchors[t], tgt.mConfig, tgt.mLeft);
                if (weights.empty())
                    continue;

                // Append the new bone, reusing the parent's inverse-bind + bounds for an exact rest pose.
                const std::size_t newIdx = bones.size();
                Rig::BoneInfo info;
                info.mName = std::string(tgt.mBoneLower);
                info.mInvBindMatrix = bones[parentIdx].mInvBindMatrix;
                info.mBoundSphere = bones[parentIdx].mBoundSphere;
                bones.push_back(info);
                names.push_back(info.mName);

                for (const auto& [vidx, w] : weights)
                {
                    Rig::BoneWeights& vw = perVertex[vidx];
                    for (auto& [bone, weight] : vw) // make room: existing bones share (1 - w)
                        weight *= (1.f - w);
                    vw.emplace_back(newIdx, w);
                }
                rigChanged = true;
            }

            if (rigChanged)
            {
                rig->setBoneInfo(std::move(bones));
                rig->setInfluences(perVertex);
                rig->reinitialize();
                if (Settings::game().mJiggleSeamWelding)
                    rig->applyJiggleSeamFeather(Settings::game().mJiggleSeamWeldThreshold);
                ++paintedMeshes;
            }
        }

        if (debug)
            Log(Debug::Warning) << "Jiggle auto-rig: resync done, painted " << paintedMeshes << " mesh(es)";
    }
}
