#include "verletclothcontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <algorithm>
#include <cmath>
#include <iterator>
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
        mBodyCapsules.clear();
        mGroundNode = nullptr;
        mBodyCollisionInitialized = true;

        osg::NodePathList paths = node->getParentalNodePaths();
        if (paths.empty() || paths[0].empty())
            return;

        // Scope the lookup to this actor's own skeleton. Starting at the scene
        // root would mix identically named Bip01 bones from other actors.
        osg::Node* actorRoot = paths[0].front();
        for (auto it = paths[0].rbegin(); it != paths[0].rend(); ++it)
        {
            if (*it && Misc::StringUtils::ciEqual((*it)->getName(), "Bip01"))
            {
                actorRoot = *it;
                // The actor's placement node (Bip01's parent) sits at the feet: use it as the floor.
                if (std::next(it) != paths[0].rend())
                    mGroundNode = *std::next(it);
                break;
            }
        }

        SceneUtil::NodeMap map;
        SceneUtil::NodeMapVisitor visitor(map);
        actorRoot->accept(visitor);

        auto addChain = [&](std::initializer_list<const char*> names, float radiusScale) {
            // Consecutive bones form capsules. Missing bones are simply skipped,
            // keeping this compatible with variant rigs.
            osg::MatrixTransform* previous = nullptr;
            for (const char* name : names)
            {
                auto it = map.find(name);
                if (it == map.end() || !it->second)
                    continue;
                if (previous)
                    mBodyCapsules.push_back({ previous, it->second.get(), radiusScale });
                previous = it->second.get();
            }
        };

        // Ordered from pelvis to neck so consecutive entries form torso capsules.
        addChain({ "Bip01 Pelvis", "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2", "Bip01 Neck" }, 1.f);
        if (mSettings.mCollideLegs)
        {
            // Limbs are thinner than the torso: thigh ~0.6, calf ~0.42 of the torso radius.
            for (const char* side : { "L", "R" })
            {
                const std::string s = std::string("Bip01 ") + side;
                auto find = [&](const std::string& n) {
                    auto it = map.find(n);
                    return it != map.end() ? it->second.get() : nullptr;
                };
                osg::MatrixTransform* thigh = find(s + " Thigh");
                osg::MatrixTransform* calf = find(s + " Calf");
                osg::MatrixTransform* foot = find(s + " Foot");
                if (thigh && calf)
                    mBodyCapsules.push_back({ thigh, calf, 0.6f });
                if (calf && foot)
                    mBodyCapsules.push_back({ calf, foot, 0.42f });
            }
        }
        if (!mSettings.mGround)
            mGroundNode = nullptr;
    }

    void VerletClothController::solveBodyCollision(int pinCount, float radius, float margin)
    {
        if (mBodyCapsules.empty() || radius <= 0.f)
            return;

        auto worldPos = [](const osg::MatrixTransform* bone) -> osg::Vec3f {
            const osg::NodePathList paths = bone->getParentalNodePaths();
            if (paths.empty())
                return osg::Vec3f();
            const osg::Vec3d p = osg::computeLocalToWorld(paths[0]).getTrans();
            return osg::Vec3f(static_cast<float>(p.x()), static_cast<float>(p.y()), static_cast<float>(p.z()));
        };

        for (const BodyCapsule& capsule : mBodyCapsules)
        {
            if (!capsule.mA || !capsule.mB)
                continue;
            const osg::Vec3f a = worldPos(capsule.mA.get());
            const osg::Vec3f b = worldPos(capsule.mB.get());
            const osg::Vec3f ab = b - a;
            const float abLen2 = ab.length2();
            const float collisionRadius = std::max(0.01f, radius * capsule.mRadiusScale + margin);
            const float collisionRadius2 = collisionRadius * collisionRadius;

            for (std::size_t i = static_cast<std::size_t>(std::max(pinCount, 0)); i < mPositions.size(); ++i)
            {
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
                    // then a stable axis perpendicular to the segment.
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

                // Keep tangential motion but remove velocity aimed into the body,
                // preventing repeated tunnelling and explosive bounce at the surface.
                osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
                const float normalVelocity = velocity * outward;
                if (normalVelocity < 0.f)
                    velocity -= outward * normalVelocity;
                mPreviousPositions[i] = mPositions[i] - velocity;
            }
        }
    }

    void VerletClothController::solveGround(int pinCount)
    {
        if (!mGroundNode)
            return;
        const osg::NodePathList paths = mGroundNode->getParentalNodePaths();
        if (paths.empty())
            return;
        osg::NodePath path = paths[0];
        path.push_back(mGroundNode.get());
        const float groundZ = osg::computeLocalToWorld(path).getTrans().z() + 0.5f;

        for (std::size_t i = static_cast<std::size_t>(std::max(pinCount, 0)); i < mPositions.size(); ++i)
        {
            if (mPositions[i].z() >= groundZ)
                continue;
            mPositions[i].z() = groundZ;
            // Resting contact: no bounce, and floor friction bleeds off sliding.
            osg::Vec3f velocity = (mPositions[i] - mPreviousPositions[i]) * 0.5f;
            velocity.z() = std::max(velocity.z(), 0.f);
            mPreviousPositions[i] = mPositions[i] - velocity;
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
        const float idleDamping = Settings::game().mVerletIdleDamping;
        const bool idleWind = Settings::game().mVerletIdleWind;
        const int pinCount = std::clamp(mSettings.mPinCount > 0 ? mSettings.mPinCount
                                                                : Settings::game().mVerletPinCount.get(),
            1, static_cast<int>(mPositions.size()) - 1);
        const bool bodyCollision = Settings::game().mVerletBodyCollision;
        const float bodyCollisionRadius = Settings::game().mVerletBodyCollisionRadius;
        const float bodyCollisionMargin = Settings::game().mVerletBodyCollisionMargin;

        const float subDt = static_cast<float>(dt / static_cast<double>(substeps));
        const float anchorSpeed = static_cast<float>(anchorDelta.length() / std::max(dt, 1e-6));
        const bool stationary = anchorSpeed < 0.35f;
        const float effectiveFrictionBase = stationary ? std::min(frictionBase, idleDamping) : frictionBase;
        const float friction = std::pow(effectiveFrictionBase, subDt * sReferenceFps);
        const float effectiveWindStrength = (!idleWind && stationary) ? 0.f : windStrength;

        // The attachment point is animated with the actor.  Carry every free
        // particle by that same world-space displacement before simulating its
        // lag.  Leaving the free particles at their old world positions while
        // only pinning the root stretches the entire chain on every walking
        // frame; the distance solver then straightens it into the familiar
        // "shoot backwards / flat cape" failure.
        for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
        {
            mPositions[i] += anchorDelta;
            mPreviousPositions[i] += anchorDelta;
        }

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

                // Positions and their history were translated by anchorDelta above.
                // Applying an additional root-motion force here double-counts walking
                // and running, continuously pulling the chain backward until its
                // distance constraints solve it into a nearly flat line. Actor
                // translation therefore must not contribute a second force.
                osg::Vec3f step = velocity + acceleration * (subDt * subDt);
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
                solveGround(pinCount);
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
