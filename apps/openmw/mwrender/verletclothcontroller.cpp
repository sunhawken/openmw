#include "verletclothcontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>
#include <utility>

namespace MWRender
{
    namespace
    {
        constexpr double sMaxDeltaTime = 0.1;
        constexpr float sTeleportResetDistance = 128.f;
        constexpr float sReferenceFps = 30.f;

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

        void setNodeMatrix(osg::MatrixTransform* node, const osg::Matrix& matrix)
        {
            if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
            {
                // NifOsg::MatrixTransform exposes translation as its fast path, but
                // preserve the authored rotation/scale too for ordinary OSG nodes.
                nifTransform->setTranslation(matrix.getTrans());
            }
            else
                node->setMatrix(matrix);
        }
    }

    VerletClothController::VerletClothController(
        std::vector<osg::ref_ptr<osg::MatrixTransform>> chain, VerletClothSettings settings, bool debug)
        : mChain(std::move(chain))
        , mSettings(std::move(settings))
        , mDebug(debug)
    {
    }

    std::vector<osg::Vec3f> VerletClothController::restWorldPositions(const osg::Matrix& rootParentWorld) const
    {
        std::vector<osg::Vec3f> positions;
        positions.reserve(mRestLocalMatrices.size());

        osg::Matrix parentWorld = rootParentWorld;
        for (const osg::Matrix& local : mRestLocalMatrices)
        {
            positions.push_back(local.getTrans() * parentWorld);
            parentWorld = local * parentWorld;
        }
        return positions;
    }

    void VerletClothController::initialize(const osg::Matrix& rootParentWorld, double simTime)
    {
        mRestLocalMatrices.clear();
        mRestLocalMatrices.reserve(mChain.size());
        for (const osg::ref_ptr<osg::MatrixTransform>& node : mChain)
            mRestLocalMatrices.push_back(node->getMatrix());

        mPositions = restWorldPositions(rootParentWorld);
        mPreviousPositions = mPositions;

        mSegmentLengths.clear();
        if (mPositions.size() > 1)
        {
            mSegmentLengths.reserve(mPositions.size() - 1);
            for (std::size_t i = 1; i < mPositions.size(); ++i)
                mSegmentLengths.push_back((mPositions[i] - mPositions[i - 1]).length());
        }

        mPreviousAnchor = mPositions.empty() ? osg::Vec3f() : mPositions.front();
        mLastSimTime = simTime;
        mInitialized = true;
    }

    void VerletClothController::resetToRest(const osg::Matrix& rootParentWorld, double simTime)
    {
        mPositions = restWorldPositions(rootParentWorld);
        mPreviousPositions = mPositions;
        if (!mPositions.empty())
            mPreviousAnchor = mPositions.front();
        mLastSimTime = simTime;
        writeBoneTransforms(rootParentWorld);
    }

    void VerletClothController::writeBoneTransforms(const osg::Matrix& rootParentWorld)
    {
        osg::Matrix parentWorld = rootParentWorld;

        for (std::size_t i = 0; i < mChain.size() && i < mPositions.size(); ++i)
        {
            osg::Matrix local = mRestLocalMatrices[i];
            const osg::Vec3f localTranslation = mPositions[i] * osg::Matrix::inverse(parentWorld);
            local.setTrans(localTranslation);
            setNodeMatrix(mChain[i].get(), local);
            parentWorld = local * parentWorld;
        }
    }

    void VerletClothController::initializeBodyCollisionNodes(osg::MatrixTransform* node)
    {
        mBodyCollisionNodes.clear();
        mBodyCollisionInitialized = true;

        osg::NodePathList paths = node->getParentalNodePaths();
        if (paths.empty() || paths[0].empty())
            return;

        osg::Node* actorRoot = paths[0].front();
        SceneUtil::NodeMap map;
        SceneUtil::NodeMapVisitor visitor(map);
        actorRoot->accept(visitor);

        // Ordered from pelvis to neck so consecutive entries form torso capsules.
        // Missing bones are simply skipped, keeping this compatible with variant rigs.
        for (const char* name : { "Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck" })
        {
            auto it = map.find(name);
            if (it != map.end() && it->second)
                mBodyCollisionNodes.push_back(it->second);
        }
    }

    std::vector<osg::Vec3f> VerletClothController::bodyCollisionPoints() const
    {
        std::vector<osg::Vec3f> points;
        points.reserve(mBodyCollisionNodes.size());

        for (const osg::ref_ptr<osg::MatrixTransform>& bone : mBodyCollisionNodes)
        {
            if (!bone)
                continue;
            const osg::NodePathList paths = bone->getParentalNodePaths();
            if (paths.empty())
                continue;
            points.push_back(osg::computeLocalToWorld(paths[0]).getTrans());
        }
        return points;
    }

    void VerletClothController::solveBodyCollision(int pinCount, float radius, float margin)
    {
        if (mBodyCollisionNodes.size() < 2 || radius <= 0.f)
            return;

        const std::vector<osg::Vec3f> points = bodyCollisionPoints();
        if (points.size() < 2)
            return;

        const float collisionRadius = std::max(0.01f, radius + margin);
        const float collisionRadius2 = collisionRadius * collisionRadius;

        for (std::size_t i = static_cast<std::size_t>(std::max(pinCount, 0)); i < mPositions.size(); ++i)
        {
            for (std::size_t seg = 1; seg < points.size(); ++seg)
            {
                const osg::Vec3f a = points[seg - 1];
                const osg::Vec3f b = points[seg];
                const osg::Vec3f ab = b - a;
                const float abLen2 = ab.length2();

                float t = 0.f;
                if (abLen2 > 1e-6f)
                    t = std::clamp(((mPositions[i] - a) * ab) / abLen2, 0.f, 1.f);

                const osg::Vec3f closest = a + ab * t;
                osg::Vec3f outward = mPositions[i] - closest;
                const float dist2 = outward.length2();
                if (dist2 >= collisionRadius2)
                    continue;

                float dist = std::sqrt(std::max(dist2, 1e-10f));
                if (dist <= 1e-5f)
                {
                    // Degenerate exact-center case: use the current motion direction first,
                    // then a stable axis perpendicular to the torso segment.
                    outward = mPositions[i] - mPreviousPositions[i];
                    if (outward.length2() <= 1e-8f)
                    {
                        outward = ab ^ osg::Vec3f(0.f, 0.f, 1.f);
                        if (outward.length2() <= 1e-8f)
                            outward = osg::Vec3f(0.f, 1.f, 0.f);
                    }
                    outward.normalize();
                    dist = 0.f;
                }
                else
                    outward /= dist;

                const osg::Vec3f correction = outward * (collisionRadius - dist);
                mPositions[i] += correction;

                // Keep tangential motion but remove velocity aimed into the torso,
                // preventing repeated tunnelling and explosive bounce at the surface.
                osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
                const float normalVelocity = velocity * outward;
                if (normalVelocity < 0.f)
                    velocity -= outward * normalVelocity;
                mPreviousPositions[i] = mPositions[i] - velocity;
            }
        }
    }

    void VerletClothController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        if (mChain.size() < 2 || !mSettings.mEnabled)
        {
            traverse(node, nv);
            return;
        }

        const osg::Matrix rootParentWorld = parentWorldMatrixFor(node);
        const double simTime = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;

        if (!mInitialized)
        {
            initialize(rootParentWorld, simTime);
            if (!mBodyCollisionInitialized)
                initializeBodyCollisionNodes(node);
            traverse(node, nv);
            return;
        }

        const std::vector<osg::Vec3f> restPositions = restWorldPositions(rootParentWorld);
        if (restPositions.size() != mPositions.size())
        {
            resetToRest(rootParentWorld, simTime);
            traverse(node, nv);
            return;
        }

        const osg::Vec3f anchor = restPositions.front();
        const osg::Vec3f anchorDelta = anchor - mPreviousAnchor;
        if (anchorDelta.length2() > sTeleportResetDistance * sTeleportResetDistance)
        {
            resetToRest(rootParentWorld, simTime);
            traverse(node, nv);
            return;
        }
        mPreviousAnchor = anchor;

        double dt = simTime - mLastSimTime;
        mLastSimTime = simTime;
        if (dt <= 0.0)
        {
            traverse(node, nv);
            return;
        }
        dt = std::min(dt, sMaxDeltaTime);

        // NIF metadata is the normal source of cloth tuning. The Verlet tab can
        // optionally override it live for rapid in-game iteration.
        if (!Settings::game().mVerletEnabled)
        {
            resetToRest(rootParentWorld, simTime);
            traverse(node, nv);
            return;
        }

        const bool useGlobal = Settings::game().mVerletUseGlobalSettings;
        const int substeps = std::clamp(useGlobal ? Settings::game().mVerletSubsteps.get() : mSettings.mSubsteps, 1, 8);
        const int iterations
            = std::clamp(useGlobal ? Settings::game().mVerletIterations.get() : mSettings.mIterations, 1, 32);
        const float gravity = useGlobal ? Settings::game().mVerletGravity.get() : mSettings.mGravity;
        const float windStrength = useGlobal ? Settings::game().mVerletWindStrength.get() : mSettings.mWindStrength;
        const float windFrequency
            = useGlobal ? Settings::game().mVerletWindFrequency.get() : mSettings.mWindFrequency;
        const float frictionBase
            = std::clamp(useGlobal ? Settings::game().mVerletFriction.get() : mSettings.mFriction, 0.f, 1.f);
        const float maxStep
            = std::max(0.01f, useGlobal ? Settings::game().mVerletMaxStep.get() : mSettings.mMaxStep);
        const float movementInfluence = Settings::game().mVerletMovementInfluence;
        const float idleDamping = Settings::game().mVerletIdleDamping;
        const bool idleWind = Settings::game().mVerletIdleWind;
        const int pinCount = std::clamp(Settings::game().mVerletPinCount.get(), 1,
            static_cast<int>(mPositions.size()) - 1);
        const bool bodyCollision = Settings::game().mVerletBodyCollision;
        const float bodyCollisionRadius = Settings::game().mVerletBodyCollisionRadius;
        const float bodyCollisionMargin = Settings::game().mVerletBodyCollisionMargin;

        const float subDt = static_cast<float>(dt / static_cast<double>(substeps));
        const float anchorSpeed = static_cast<float>(anchorDelta.length() / std::max(dt, 1e-6));
        const bool stationary = anchorSpeed < 0.35f;
        const float effectiveFrictionBase = stationary ? std::min(frictionBase, idleDamping) : frictionBase;
        const float friction = std::pow(effectiveFrictionBase, subDt * sReferenceFps);
        const float effectiveWindStrength = (!idleWind && stationary) ? 0.f : windStrength;

        for (int substep = 0; substep < substeps; ++substep)
        {
            // Pin the authored attachment region. For capes this keeps the
            // shoulder/upper-arm area from being dragged by the lower cloth.
            for (int i = 0; i < pinCount; ++i)
            {
                mPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
                mPreviousPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
            }

            for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
            {
                const osg::Vec3f current = mPositions[i];
                osg::Vec3f velocity = (mPositions[i] - mPreviousPositions[i]) * friction;

                const float phase
                    = static_cast<float>(simTime) * windFrequency + static_cast<float>(i) * 0.47f;
                const osg::Vec3f acceleration(
                    std::sin(phase * 6.28318530718f) * effectiveWindStrength,
                    std::sin(phase * 4.117f + 0.9f) * effectiveWindStrength * 0.35f,
                    -gravity);

                // Character motion should pull the cape opposite the root's movement.
                // Apply a fraction of the per-frame root displacement across substeps;
                // distal particles trail slightly more than particles near the pin.
                const float chainT = static_cast<float>(i) / static_cast<float>(mPositions.size() - 1);
                const osg::Vec3f movementDrag
                    = anchorDelta * (-movementInfluence * chainT / static_cast<float>(substeps));
                osg::Vec3f step = velocity + acceleration * (subDt * subDt) + movementDrag;
                if (step.length2() > maxStep * maxStep)
                {
                    step.normalize();
                    step *= maxStep;
                }

                mPreviousPositions[i] = current;
                mPositions[i] += step;
            }

            // Position-based distance constraints. This is the 3D bone-chain
            // equivalent of the source project's Polygon::ConstraintPolygon().
            for (int iteration = 0; iteration < iterations; ++iteration)
            {
                for (int pinned = 0; pinned < pinCount; ++pinned)
                    mPositions[static_cast<std::size_t>(pinned)] = restPositions[static_cast<std::size_t>(pinned)];

                for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                {
                    osg::Vec3f delta = mPositions[i] - mPositions[i - 1];
                    const float distance = delta.length();
                    if (distance <= 1e-5f)
                        continue;

                    const float restLength = mSegmentLengths[i - 1];
                    const osg::Vec3f correction = delta * ((distance - restLength) / distance);

                    if (i == static_cast<std::size_t>(pinCount))
                    {
                        // The previous particle is pinned, so the first free
                        // particle takes the entire correction.
                        mPositions[i] -= correction;
                    }
                    else
                    {
                        mPositions[i - 1] += correction * 0.5f;
                        mPositions[i] -= correction * 0.5f;
                    }
                }

                if (bodyCollision)
                    solveBodyCollision(pinCount, bodyCollisionRadius, bodyCollisionMargin);
            }
        }

        writeBoneTransforms(rootParentWorld);

        if (mDebug && (mDebugCounter++ % 60) == 0)
        {
            const float tipDistance = (mPositions.back() - restPositions.back()).length();
            Log(Debug::Info) << "Verlet cloth: root=" << node->getName() << " particles=" << mPositions.size()
                             << " tip displacement=" << tipDistance << " friction=" << frictionBase
                             << " gravity=" << gravity << " wind=" << effectiveWindStrength
                             << " rootSpeed=" << anchorSpeed << " pinCount=" << pinCount
                             << " bodyCollision=" << bodyCollision << " bodyRadius=" << bodyCollisionRadius
                             << " globalOverride=" << useGlobal;
        }

        traverse(node, nv);
    }
}
