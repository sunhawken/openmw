#include "verletclothcontroller.hpp"

#include <components/misc/strings/algorithm.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>

namespace MWRender
{
    namespace
    {
        // physics-verlet runs at a fixed 60 Hz step; gravity is 9.8 m/s^2 in game units
        // (the Siff character is about 131 units = 1.81 m, so 72.5 units per metre).
        constexpr float sStep = 1.f / 60.f;
        constexpr float sGravity = 9.81f * 72.5f;
        constexpr int sIterations = 10;
        constexpr float sChainMax = 1.f;    // rope.c: neighbours at most their rest distance apart
        constexpr float sLateralMax = 1.1f; // cloth.c: neighbours at most 1.1 x their rest distance apart
        constexpr float sParticleRadius = 0.75f;
        constexpr float sTeleportDistance = 128.f;
        constexpr float sMaxCatchUp = 0.25f;
        // immovable body colliders (units): torso, thigh, calf radii plus a little padding
        constexpr float sTorsoRadius = 12.f + 1.5f;
        constexpr float sThighRadius = 7.2f + 1.5f;
        constexpr float sCalfRadius = 5.f + 1.5f;

        osg::Matrix parentWorldMatrixFor(osg::MatrixTransform* node)
        {
            osg::Matrix parentWorld;
            osg::NodePathList paths = node->getParentalNodePaths();
            if (!paths.empty() && paths[0].size() > 1)
            {
                osg::NodePath path = paths[0];
                path.pop_back();
                parentWorld = osg::computeLocalToWorld(path);
            }
            return parentWorld;
        }

        void setNodeTranslation(osg::MatrixTransform* node, const osg::Matrix& matrix)
        {
            if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
                nifTransform->setTranslation(matrix.getTrans());
            else
                node->setMatrix(matrix);
        }

        osg::Vec3f worldPosition(const osg::MatrixTransform* bone)
        {
            const osg::NodePathList paths = bone->getParentalNodePaths();
            if (paths.empty())
                return osg::Vec3f();
            const osg::Vec3d p = osg::computeLocalToWorld(paths[0]).getTrans();
            return osg::Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
        }
    }

    VerletClothController::VerletClothController(
        std::vector<std::vector<osg::ref_ptr<osg::MatrixTransform>>> chains, int pinCount, bool debug)
        : mPinCount(std::max(1, pinCount))
        , mDebug(debug)
    {
        for (auto& bones : chains)
        {
            Chain chain;
            chain.mBones = std::move(bones);
            mChains.push_back(std::move(chain));
        }
    }

    void VerletClothController::computeRest(Chain& chain) const
    {
        chain.mRest.resize(chain.mBones.size());
        osg::Matrix parentWorld = parentWorldMatrixFor(chain.mBones.front().get());
        for (std::size_t i = 0; i < chain.mBones.size(); ++i)
        {
            chain.mRest[i] = chain.mRestLocal[i].getTrans() * parentWorld;
            parentWorld = chain.mRestLocal[i] * parentWorld;
        }
    }

    void VerletClothController::initialize(osg::MatrixTransform* node)
    {
        mInitialized = true;
        for (Chain& chain : mChains)
        {
            chain.mRestLocal.clear();
            for (const auto& bone : chain.mBones)
                chain.mRestLocal.push_back(bone->getMatrix());
            computeRest(chain);
            chain.mSegment.assign(chain.mBones.size(), 0.f);
            for (std::size_t i = 1; i < chain.mBones.size(); ++i)
                chain.mSegment[i] = (chain.mRest[i] - chain.mRest[i - 1]).length();
        }

        // cloth.c: link every chain to its two nearest neighbours (measured a few bones below the root)
        auto probe = [&](const Chain& c) { return c.mRest[std::min<std::size_t>(mPinCount, c.mRest.size() - 1)]; };
        std::vector<std::pair<std::size_t, std::size_t>> pairs;
        for (std::size_t a = 0; a < mChains.size(); ++a)
        {
            std::vector<std::pair<float, std::size_t>> near;
            for (std::size_t b = 0; b < mChains.size(); ++b)
                if (a != b)
                    near.emplace_back((probe(mChains[a]) - probe(mChains[b])).length(), b);
            std::sort(near.begin(), near.end());
            for (std::size_t k = 0; k < near.size() && k < 2; ++k)
            {
                const auto pair = std::make_pair(std::min(a, near[k].second), std::max(a, near[k].second));
                if (std::find(pairs.begin(), pairs.end(), pair) == pairs.end())
                    pairs.push_back(pair);
            }
        }
        for (const auto& [a, b] : pairs)
        {
            Link link{ a, b, {} };
            const std::size_t levels = std::min(mChains[a].mRest.size(), mChains[b].mRest.size());
            for (std::size_t l = 0; l < levels; ++l)
                link.mRest.push_back((mChains[a].mRest[l] - mChains[b].mRest[l]).length());
            mLinks.push_back(std::move(link));
        }

        // body colliders from this actor's own skeleton, and the actor's floor
        osg::NodePathList paths = node->getParentalNodePaths();
        if (!paths.empty() && !paths[0].empty())
        {
            osg::Node* actorRoot = paths[0].front();
            for (auto it = paths[0].rbegin(); it != paths[0].rend(); ++it)
            {
                if (*it && Misc::StringUtils::ciEqual((*it)->getName(), "Bip01"))
                {
                    actorRoot = *it;
                    if (std::next(it) != paths[0].rend())
                        mGroundNode = *std::next(it);
                    break;
                }
            }
            SceneUtil::NodeMap map;
            SceneUtil::NodeMapVisitor visitor(map);
            actorRoot->accept(visitor);
            auto find = [&](const std::string& name) -> osg::MatrixTransform* {
                auto it = map.find(name);
                return it != map.end() ? it->second.get() : nullptr;
            };
            auto addChain = [&](std::initializer_list<const char*> names, float radius) {
                osg::MatrixTransform* previous = nullptr;
                for (const char* name : names)
                {
                    osg::MatrixTransform* bone = find(name);
                    if (!bone)
                        continue;
                    if (previous)
                        mCapsules.push_back({ previous, bone, radius });
                    previous = bone;
                }
            };
            addChain({ "Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck" }, sTorsoRadius);
            for (const char* side : { "L", "R" })
            {
                const std::string s = std::string("Bip01 ") + side;
                osg::MatrixTransform* thigh = find(s + " Thigh");
                osg::MatrixTransform* calf = find(s + " Calf");
                osg::MatrixTransform* foot = find(s + " Foot");
                if (thigh && calf)
                    mCapsules.push_back({ thigh, calf, sThighRadius });
                if (calf && foot)
                    mCapsules.push_back({ calf, foot, sCalfRadius });
            }
        }
        resetToRest();
    }

    void VerletClothController::resetToRest()
    {
        for (Chain& chain : mChains)
        {
            chain.mPos = chain.mRest;
            chain.mOld = chain.mRest;
        }
        mAccumulator = 0.f;
    }

    void VerletClothController::solveCollisions()
    {
        const int pin = mPinCount;
        // world_collide: overlapping particles are moved apart, half each
        for (std::size_t ca = 0; ca < mChains.size(); ++ca)
            for (std::size_t i = pin; i < mChains[ca].mPos.size(); ++i)
                for (std::size_t cb = ca; cb < mChains.size(); ++cb)
                    for (std::size_t j = (cb == ca ? i + 2 : pin); j < mChains[cb].mPos.size(); ++j)
                    {
                        osg::Vec3f& p = mChains[ca].mPos[i];
                        osg::Vec3f& q = mChains[cb].mPos[j];
                        osg::Vec3f d = p - q;
                        const float dist = d.length();
                        const float min = 2.f * sParticleRadius;
                        if (dist < min && dist > 1e-6f)
                        {
                            const osg::Vec3f adjust = d * ((min - dist) * 0.5f / dist);
                            p += adjust;
                            q -= adjust;
                        }
                    }
        // immovable body capsules and the floor
        for (Chain& chain : mChains)
            for (std::size_t i = pin; i < chain.mPos.size(); ++i)
            {
                osg::Vec3f& p = chain.mPos[i];
                for (std::size_t c = 0; c < mCapsules.size(); ++c)
                {
                    const osg::Vec3f a = mCapA[c], b = mCapB[c];
                    const osg::Vec3f ab = b - a;
                    const float len2 = ab.length2();
                    const float t = len2 > 1e-6f ? std::clamp(((p - a) * ab) / len2, 0.f, 1.f) : 0.f;
                    const osg::Vec3f q = p - (a + ab * t);
                    const float dist = q.length();
                    if (dist < mCapsules[c].mRadius && dist > 1e-6f)
                        p += q * ((mCapsules[c].mRadius - dist) / dist);
                }
                if (mHasGround && p.z() < mGroundZ + 0.8f)
                {
                    p.z() = mGroundZ + 0.8f;
                    chain.mOld[i].z() = std::min(chain.mOld[i].z(), p.z());
                }
            }
    }

    void VerletClothController::step(float dt)
    {
        const int pin = mPinCount;
        // world_update_positions + world_apply_gravity
        for (Chain& chain : mChains)
            for (std::size_t i = pin; i < chain.mPos.size(); ++i)
            {
                const osg::Vec3f velocity = chain.mPos[i] - chain.mOld[i];
                chain.mOld[i] = chain.mPos[i];
                chain.mPos[i] += velocity + osg::Vec3f(0.f, 0.f, -sGravity) * (dt * dt);
            }

        for (int it = 0; it < sIterations; ++it)
        {
            solveCollisions();
            for (Chain& chain : mChains)
            {
                // constrain_distance_from_point(maxd = 0): pinned particles sit on their animated position
                for (int i = 0; i < pin && i < static_cast<int>(chain.mPos.size()); ++i)
                    chain.mPos[i] = chain.mRest[i];
                // constrain_distance_between_objects(i, i + 1, rest)
                for (std::size_t i = pin; i < chain.mPos.size(); ++i)
                {
                    osg::Vec3f d = chain.mPos[i] - chain.mPos[i - 1];
                    const float dist = d.length();
                    const float maxd = chain.mSegment[i] * sChainMax;
                    if (dist > maxd && dist > 1e-6f)
                    {
                        const osg::Vec3f adjust = d * ((dist - maxd) * 0.5f / dist);
                        chain.mPos[i] -= adjust;
                        chain.mPos[i - 1] += adjust;
                    }
                }
            }
            for (const Link& link : mLinks)
            {
                Chain& a = mChains[link.mA];
                Chain& b = mChains[link.mB];
                for (std::size_t l = pin; l < link.mRest.size(); ++l)
                {
                    osg::Vec3f d = a.mPos[l] - b.mPos[l];
                    const float dist = d.length();
                    const float maxd = link.mRest[l] * sLateralMax;
                    if (dist > maxd && dist > 1e-6f)
                    {
                        const osg::Vec3f adjust = d * ((dist - maxd) * 0.5f / dist);
                        a.mPos[l] -= adjust;
                        b.mPos[l] += adjust;
                    }
                }
            }
        }
    }

    void VerletClothController::writeBones()
    {
        for (Chain& chain : mChains)
        {
            osg::Matrix parentWorld = parentWorldMatrixFor(chain.mBones.front().get());
            for (std::size_t i = 0; i < chain.mBones.size(); ++i)
            {
                osg::Matrix local = chain.mRestLocal[i];
                local.setTrans(chain.mPos[i] * osg::Matrix::inverse(parentWorld));
                setNodeTranslation(chain.mBones[i].get(), local);
                parentWorld = local * parentWorld;
            }
        }
    }

    void VerletClothController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        const double time = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;
        if (!mInitialized)
        {
            initialize(node);
            mLastTime = time;
            traverse(node, nv);
            return;
        }

        // animated positions of every chain for this frame
        const osg::Vec3f previousRoot = mChains.front().mRest.front();
        for (Chain& chain : mChains)
            computeRest(chain);
        const double dt = time - mLastTime;
        mLastTime = time;
        if (dt <= 0.0)
        {
            traverse(node, nv);
            return;
        }
        if (!Settings::game().mVerletEnabled || dt > sMaxCatchUp
            || (mChains.front().mRest.front() - previousRoot).length() > sTeleportDistance)
        {
            resetToRest();
            writeBones();
            traverse(node, nv);
            return;
        }

        mCapA.resize(mCapsules.size());
        mCapB.resize(mCapsules.size());
        for (std::size_t c = 0; c < mCapsules.size(); ++c)
        {
            mCapA[c] = worldPosition(mCapsules[c].mA.get());
            mCapB[c] = worldPosition(mCapsules[c].mB.get());
        }
        mHasGround = mGroundNode != nullptr;
        if (mHasGround)
        {
            const osg::NodePathList paths = mGroundNode->getParentalNodePaths();
            if (!paths.empty())
                mGroundZ = static_cast<float>(osg::computeLocalToWorld(paths[0]).getTrans().z());
            else
                mHasGround = false;
        }

        mAccumulator += static_cast<float>(dt);
        int steps = 0;
        while (mAccumulator >= sStep && steps < 4)
        {
            step(sStep);
            mAccumulator -= sStep;
            ++steps;
        }
        if (steps == 4)
            mAccumulator = 0.f;
        writeBones();
        traverse(node, nv);
    }
}
