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

#include <components/debug/debuglog.hpp>
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

    void JiggleAutoRig::run(osg::Group* objectRoot)
    {
        if (!objectRoot || !Settings::game().mJiggleAutoRig)
            return;
        const bool debug = Settings::game().mJiggleAutoRigDebug;

        RigCollector rc;
        objectRoot->accept(rc);
        if (rc.mRigs.empty())
            return;

        for (Rig* rig : rc.mRigs)
            for (const std::string& name : rig->getInfluenceBoneNames())
                for (std::string_view jb : sJiggleBones)
                    if (name == jb)
                    {
                        if (debug)
                            Log(Debug::Warning) << "Jiggle auto-rig: body already rigged; skipping";
                        return;
                    }

        SkeletonFinder sf;
        objectRoot->accept(sf);
        if (!sf.mSkeleton)
            return;
        SceneUtil::Skeleton* skeleton = sf.mSkeleton;

        // 1) Create the jiggle bone nodes (identity children of their weighted parent bone) and
        //    attach the spring controller. Anchors are computed for weight painting only.
        std::array<std::optional<osg::Vec3f>, 4> anchors;
        bool addedAny = false;
        for (std::size_t t = 0; t < sTargets.size(); ++t)
        {
            const Target& tgt = sTargets[t];
            const auto lz = landmarkZ(rc.mRigs, tgt.mConfig.mLandmark);
            if (!lz)
                continue;
            anchors[t] = findAnchor(rc.mRigs, tgt.mConfig, tgt.mLeft, *lz);
            if (!anchors[t])
                continue;

            SceneUtil::Bone* parent = skeleton->getBone(std::string(tgt.mConfig.mParent));
            if (!parent || !parent->mNode)
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
            parent->mNode->addChild(boneNode);
            boneNode->addUpdateCallback(new JiggleBoneController(debug));
            addedAny = true;
            if (debug)
                Log(Debug::Warning) << "Jiggle auto-rig: added bone " << tgt.mBoneNode << " under "
                                    << tgt.mConfig.mParent << " anchor " << anchors[t]->x() << "," << anchors[t]->y()
                                    << "," << anchors[t]->z();
        }
        if (!addedAny)
            return;

        // Rebuild the bone cache/hierarchy so the new bones resolve by name.
        skeleton->markDirty();

        // 2) Paint weights: for each body mesh that weights a target's parent, add the jiggle bone
        //    (using that mesh's own parent inverse-bind, so the rest pose is exact) + cone weights.
        for (Rig* rig : rc.mRigs)
        {
            const osg::Vec3Array* verts = sourceVerts(*rig);
            if (!verts || verts->empty())
                continue;
            std::vector<std::string> names = rig->getInfluenceBoneNames();
            std::vector<Rig::BoneInfo> bones = rig->getBoneInfoList();
            std::vector<Rig::BoneWeights> perVertex = rig->getPerVertexInfluences(verts->size());

            bool rigChanged = false;
            for (std::size_t t = 0; t < sTargets.size(); ++t)
            {
                if (!anchors[t])
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
            }
        }

        if (debug)
            Log(Debug::Warning) << "Jiggle auto-rig: done";
    }
}
