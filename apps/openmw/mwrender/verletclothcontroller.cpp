#include "verletclothcontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/sceneutil/visitor.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>
#include <osg/Quat>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace MWRender
{
    namespace
    {
        constexpr double sMaxDeltaTime = 0.05;
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
        mGroundContacts.assign(mPositions.size(), 0);
        mBodyContacts.clear();

        mSegmentLengths.clear();
        if (mPositions.size() > 1)
        {
            mSegmentLengths.reserve(mPositions.size() - 1);
            for (std::size_t i = 1; i < mPositions.size(); ++i)
                mSegmentLengths.push_back((mPositions[i] - mPositions[i - 1]).length());
        }

        mPreviousAnchor = mPositions.empty() ? osg::Vec3f() : mPositions.front();
        mPreviousRootParentWorld = rootParentWorld;
        mPreviousRootVelocity.set(0.f, 0.f, 0.f);
        mFilteredRootAcceleration.set(0.f, 0.f, 0.f);
        mFilteredRootVelocity.set(0.f, 0.f, 0.f);
        mPreviousStep = 0.f;
        mRootVelocityInitialized = false;
        mSleeping = false;
        mSleepTime = 0.f;
        mSleepWindowTime = 0.f;
        mSleepMinPositions.clear();
        mSleepMaxPositions.clear();
        mLastSimTime = simTime;
        mInitialized = true;
    }

    void VerletClothController::resetToRest(const osg::Matrix& rootParentWorld, double simTime)
    {
        mPositions = restWorldPositions(rootParentWorld);
        mPreviousPositions = mPositions;
        mGroundContacts.assign(mPositions.size(), 0);
        mBodyContacts.clear();
        if (!mPositions.empty())
            mPreviousAnchor = mPositions.front();
        mPreviousRootParentWorld = rootParentWorld;
        mPreviousRootVelocity.set(0.f, 0.f, 0.f);
        mFilteredRootAcceleration.set(0.f, 0.f, 0.f);
        mFilteredRootVelocity.set(0.f, 0.f, 0.f);
        mPreviousStep = 0.f;
        mRootVelocityInitialized = false;
        mSleeping = false;
        mSleepTime = 0.f;
        mSleepWindowTime = 0.f;
        mSleepMinPositions.clear();
        mSleepMaxPositions.clear();
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

    void VerletClothController::updateCollisionWorld(
        const std::vector<osg::Vec3f>& restPositions, float radius, float margin)
    {
        mWorldCapsules.clear();
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
            WorldCapsule world{worldPos(capsule.mA.get()), worldPos(capsule.mB.get()), capsule.mRadiusScale, {}};
            if (mSettings.mRestCollisionFit)
            {
                const osg::Vec3f ab = world.mB - world.mA;
                const float len2 = ab.length2();
                const float configuredRadius = std::max(0.01f, radius * world.mRadiusScale + margin);
                world.mParticleRadii.reserve(restPositions.size());
                for (const osg::Vec3f& rest : restPositions)
                {
                    const float t = len2 > 1e-6f ? std::clamp(((rest - world.mA) * ab) / len2, 0.f, 1.f) : 0.f;
                    const float restDistance = (rest - (world.mA + ab * t)).length();
                    world.mParticleRadii.push_back(std::min(configuredRadius, std::max(0.01f, restDistance)));
                }
            }
            mWorldCapsules.push_back(std::move(world));
        }
        mHasGround = false;
        if (mBodyContacts.size() != mWorldCapsules.size())
            mBodyContacts.assign(mWorldCapsules.size(), std::vector<unsigned char>(mPositions.size(), 0));
        if (mGroundNode)
        {
            const osg::NodePathList paths = mGroundNode->getParentalNodePaths();
            if (!paths.empty())
            {
                osg::NodePath path = paths[0];
                // getParentalNodePaths already ends at the requested node.
                mGroundZ = osg::computeLocalToWorld(path).getTrans().z() + 0.5f;
                mHasGround = true;
            }
        }
    }

    void VerletClothController::solveBodyCollision(int pinCount, float radius, float margin)
    {
        if (mWorldCapsules.empty() || radius <= 0.f)
            return;
        for (std::size_t capsuleIndex = 0; capsuleIndex < mWorldCapsules.size(); ++capsuleIndex)
        {
            const WorldCapsule& capsule = mWorldCapsules[capsuleIndex];
            const osg::Vec3f a = capsule.mA;
            const osg::Vec3f b = capsule.mB;
            const osg::Vec3f ab = b - a;
            const float abLen2 = ab.length2();
            const float contactSlop = std::max(0.f, mSettings.mContactSlop);

            for (std::size_t i = static_cast<std::size_t>(std::max(pinCount, 0)); i < mPositions.size(); ++i)
            {
                const float collisionRadius = i < capsule.mParticleRadii.size() ? capsule.mParticleRadii[i]
                    : std::max(0.01f, radius * capsule.mRadiusScale + margin);
                const float activationRadius = std::max(0.01f, collisionRadius - contactSlop);
                const float activationRadius2 = activationRadius * activationRadius;
                float t = 0.f;
                if (abLen2 > 1e-6f)
                    t = std::clamp(((mPositions[i] - a) * ab) / abLen2, 0.f, 1.f);

                const osg::Vec3f closest = a + ab * t;
                osg::Vec3f outward = mPositions[i] - closest;
                const float dist2 = outward.length2();
                const float releaseRadius = collisionRadius + contactSlop;
                const bool wasContact = mBodyContacts[capsuleIndex][i] != 0;
                if ((!wasContact && dist2 >= activationRadius2) || dist2 > releaseRadius * releaseRadius)
                {
                    mBodyContacts[capsuleIndex][i] = 0;
                    continue;
                }

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

                // Capture velocity before the positional correction. Otherwise the
                // correction itself becomes outward velocity on the next substep and
                // causes contact buzzing/jitter.
                osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
                const float releaseSpeed = std::max(0.01f, mSettings.mVelocityDeadzone) * std::max(1e-5f, mPreviousStep);
                if (wasContact && dist >= collisionRadius && velocity * outward > releaseSpeed)
                {
                    mBodyContacts[capsuleIndex][i] = 0;
                    continue;
                }
                mBodyContacts[capsuleIndex][i] = 1;

                const osg::Vec3f correction = outward * (collisionRadius - dist);
                mPositions[i] += correction;

                // Keep tangential motion but remove velocity aimed into the body.
                const float normalVelocity = velocity * outward;
                if (normalVelocity < 0.f)
                    velocity -= outward * normalVelocity;
                mPreviousPositions[i] = mPositions[i] - velocity;
            }
        }
    }

    void VerletClothController::solveGround(int pinCount)
    {
        if (!mHasGround)
            return;
        const float groundZ = mGroundZ;

        const float contactSlop = std::max(0.f, mSettings.mContactSlop);
        for (std::size_t i = static_cast<std::size_t>(std::max(pinCount, 0)); i < mPositions.size(); ++i)
        {
            // A small hysteresis band avoids repeatedly entering/leaving contact
            // from floating-point noise.
            osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
            const bool wasContact = mGroundContacts[i] != 0;
            const float releaseSpeed = std::max(0.01f, mSettings.mVelocityDeadzone) * std::max(1e-5f, mPreviousStep);
            if ((!wasContact && mPositions[i].z() >= groundZ - contactSlop)
                || mPositions[i].z() > groundZ + contactSlop
                || (wasContact && mPositions[i].z() >= groundZ && velocity.z() > releaseSpeed))
            {
                mGroundContacts[i] = 0;
                continue;
            }
            mGroundContacts[i] = 1;

            // Preserve the velocity from before the positional correction so the
            // clamp itself cannot inject a vertical bounce.
            mPositions[i].z() = groundZ;
            velocity.x() *= 0.5f;
            velocity.y() *= 0.5f;
            velocity.z() = 0.f;
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
        const osg::Vec3f previousAnchor = mPreviousAnchor;
        const osg::Vec3f anchorDelta = anchor - mPreviousAnchor;
        if (anchorDelta.length2() > sTeleportResetDistance * sTeleportResetDistance)
        {
            resetToRest(rootParentWorld, simTime);
            traverse(node, nv);
            return;
        }
        double dt = simTime - mLastSimTime;
        if (dt < 0.0 || (mSettings.mStableTiming && dt > 0.25))
        {
            // Save/load, clock rewind, or a long pause: discard stale velocity.
            resetToRest(rootParentWorld, simTime);
            traverse(node, nv);
            return;
        }
        if (dt == 0.0)
        {
            traverse(node, nv);
            return;
        }
        const double frameDt = dt;
        mLastSimTime = simTime;
        mPreviousAnchor = anchor;
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
        const int requestedSubsteps
            = std::clamp(useGlobal ? Settings::game().mVerletSubsteps.get() : mSettings.mSubsteps, 1, 8);
        const int substeps = mSettings.mStableTiming
            ? std::max(requestedSubsteps, static_cast<int>(std::ceil(dt * 240.0)))
            : requestedSubsteps;
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

        const int maxSoftRootCount = std::max(0, static_cast<int>(mPositions.size()) - pinCount);
        const int softRootCount = std::clamp(mSettings.mSoftRootCount, 0, maxSoftRootCount);
        const float softRootStrength = std::clamp(mSettings.mSoftRootStrength, 0.f, 1.f);
        const float velocityDeadzone = std::max(0.f, mSettings.mVelocityDeadzone);
        const float rotationCarry = std::clamp(mSettings.mRotationCarry, 0.f, 1.f);
        const float lateralMemory = std::clamp(mSettings.mLateralMemory, 0.f, 1.f);
        const float maxLateralDeviation = std::max(0.f, mSettings.mMaxLateralDeviation);
        const float inertia = std::clamp(mSettings.mInertia, 0.f, 1.f);
        const float airDrag = std::max(0.f, mSettings.mAirDrag);

        // Quintic smootherstep has zero slope at both ends, which removes the
        // visible rigid->physics hinge better than a linear/cubic transition.
        const auto softRootDynamicWeight = [&](std::size_t index) -> float
        {
            if (softRootCount <= 0)
                return 1.f;

            const std::size_t firstSoft = static_cast<std::size_t>(pinCount);
            const std::size_t endSoft = firstSoft + static_cast<std::size_t>(softRootCount);
            if (index < firstSoft || index >= endSoft)
                return 1.f;

            const float t = static_cast<float>(index - firstSoft + 1)
                / static_cast<float>(softRootCount + 1);
            const float smooth = t * t * t * (t * (t * 6.f - 15.f) + 10.f);
            return 1.f - softRootStrength * (1.f - smooth);
        };

        const bool bodyCollision = Settings::game().mVerletBodyCollision;
        const float bodyCollisionRadius = Settings::game().mVerletBodyCollisionRadius;
        const float bodyCollisionMargin = Settings::game().mVerletBodyCollisionMargin;

        const float subDt = static_cast<float>(dt / static_cast<double>(substeps));
        // Metadata strengths retain their authored 60-fps/six-substep response
        // but no longer grow stiffer when the renderer produces more frames.
        const float strengthExponent = mSettings.mStableTiming ? subDt * 360.f : 1.f;
        const float lateralFollow = 1.f - std::pow(1.f - lateralMemory, strengthExponent);
        osg::Vec3f inertialAcceleration;
        if (inertia > 0.f || airDrag > 0.f)
        {
            const osg::Vec3f rootDelta = rootParentWorld.getTrans() - mPreviousRootParentWorld.getTrans();
            const osg::Vec3f rootVelocity = rootDelta / static_cast<float>(frameDt);
            osg::Vec3f rootAcceleration;
            if (mRootVelocityInitialized)
                rootAcceleration = (rootVelocity - mPreviousRootVelocity) / static_cast<float>(frameDt);
            const float limit = std::max(0.f, mSettings.mInertiaMaxAcceleration);
            if (rootAcceleration.length2() > limit * limit && rootAcceleration.length2() > 0.f)
            {
                rootAcceleration.normalize();
                rootAcceleration *= limit;
            }
            const float blend = 1.f - std::exp(-static_cast<float>(frameDt) / 0.05f);
            mFilteredRootAcceleration += (rootAcceleration - mFilteredRootAcceleration) * blend;
            mFilteredRootVelocity += (rootVelocity - mFilteredRootVelocity) * blend;
            mPreviousRootVelocity = rootVelocity;
            mRootVelocityInitialized = true;
            inertialAcceleration = -mFilteredRootAcceleration * inertia;
        }
        const float anchorSpeed = static_cast<float>(anchorDelta.length() / std::max(dt, 1e-6));
        const osg::Matrix frameDelta = osg::Matrix::inverse(mPreviousRootParentWorld) * rootParentWorld;
        const float turnCos = std::clamp(static_cast<float>(
            (frameDelta(0, 0) + frameDelta(1, 1) + frameDelta(2, 2) - 1.0) * 0.5), -1.f, 1.f);
        const float turnSpeed = std::acos(turnCos) / static_cast<float>(frameDt);
        const bool stationary = anchorSpeed < 0.35f && (!mSettings.mStableTiming || turnSpeed < 0.015f);
        std::vector<osg::Vec3f> shapePositions = restPositions;
        if (airDrag > 0.f && mSettings.mAirShapeResponse > 0.f)
        {
            const osg::Vec3f airflow(-mFilteredRootVelocity.x(), -mFilteredRootVelocity.y(), 0.f);
            const float speed = airflow.length();
            if (speed > 1e-5f)
            {
                const float force = std::min(speed * airDrag, std::max(0.f, mSettings.mAirDragMaxAcceleration));
                const float angle = std::min(0.35f, std::atan2(force, std::max(1.f, gravity))
                    * std::clamp(mSettings.mAirShapeResponse, 0.f, 1.f));
                osg::Vec3f axis = osg::Vec3f(0.f, 0.f, -1.f) ^ airflow;
                axis.normalize();
                osg::Quat rotation;
                rotation.makeRotate(angle, axis);
                const osg::Matrix bend = osg::Matrix::rotate(rotation);
                const osg::Vec3f attachment = restPositions[static_cast<std::size_t>(pinCount - 1)];
                for (std::size_t i = static_cast<std::size_t>(pinCount); i < shapePositions.size(); ++i)
                    shapePositions[i] = (restPositions[i] - attachment) * bend + attachment;
            }
        }
        const float effectiveFrictionBase = stationary ? std::min(frictionBase, idleDamping) : frictionBase;
        const float friction = std::pow(effectiveFrictionBase, subDt * sReferenceFps);
        const float effectiveWindStrength = (!idleWind && stationary) ? 0.f : windStrength;

        // Carry the free particles with the animated parent frame before
        // simulating their relative motion. Translation carry is always applied;
        // optional rotation carry prevents extreme turns from shearing the shell
        // into a flat plane while still allowing inertial secondary motion.
        osg::Quat carriedRotation;
        carriedRotation.slerp(rotationCarry, osg::Quat(), frameDelta.getRotate());
        const osg::Matrix partialRotation = osg::Matrix::rotate(carriedRotation);
        for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
        {
            const osg::Vec3f translatedPosition = mPositions[i] + anchorDelta;
            const osg::Vec3f translatedPrevious = mPreviousPositions[i] + anchorDelta;

            if (rotationCarry > 0.f)
            {
                if (rotationCarry >= 1.f)
                {
                    mPositions[i] = mPositions[i] * frameDelta;
                    mPreviousPositions[i] = mPreviousPositions[i] * frameDelta;
                }
                else
                {
                    // Quaternion interpolation preserves width at a 180-degree
                    // turn; averaging two rotated positions shrinks that width.
                    mPositions[i] = (mPositions[i] - previousAnchor) * partialRotation + anchor;
                    mPreviousPositions[i] = (mPreviousPositions[i] - previousAnchor) * partialRotation + anchor;
                }
            }
            else
            {
                mPositions[i] = translatedPosition;
                mPreviousPositions[i] = translatedPrevious;
            }
        }
        mPreviousRootParentWorld = rootParentWorld;

        // The animated body does not change within this controller's substeps.
        // Cache collider matrices once instead of walking the scene per constraint.
        const std::vector<WorldCapsule> previousCapsules = mWorldCapsules;
        const float previousGroundZ = mGroundZ;
        const bool hadGround = mHasGround;
        updateCollisionWorld(restPositions, bodyCollisionRadius, bodyCollisionMargin);
        const float colliderThreshold2 = static_cast<float>(frameDt * frameDt) * 0.35f * 0.35f;
        bool colliderMoved = previousCapsules.size() != mWorldCapsules.size() || hadGround != mHasGround;
        for (std::size_t i = 0; !colliderMoved && i < mWorldCapsules.size(); ++i)
            colliderMoved = (previousCapsules[i].mA - mWorldCapsules[i].mA).length2() > colliderThreshold2
                || (previousCapsules[i].mB - mWorldCapsules[i].mB).length2() > colliderThreshold2;
        if (mHasGround && std::abs(mGroundZ - previousGroundZ) > std::sqrt(colliderThreshold2))
            colliderMoved = true;
        const bool canSleep = mSettings.mSleepSpeed > 0.f && stationary && !colliderMoved
            && std::abs(effectiveWindStrength) < 1e-6f && !useGlobal;
        if (mSleeping && canSleep)
        {
            for (int i = 0; i < pinCount; ++i)
            {
                mPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
                mPreviousPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
            }
            writeBoneTransforms(rootParentWorld);
            traverse(node, nv);
            return;
        }
        if (!canSleep)
        {
            mSleeping = false;
            mSleepTime = 0.f;
            mSleepWindowTime = 0.f;
            mSleepMinPositions.clear();
            mSleepMaxPositions.clear();
        }
        const std::vector<osg::Vec3f> frameStartPositions = mPositions;
        // If authored tips extend below the floor, they need room to drape
        // sideways. A uniform guard can otherwise trap a long chain between
        // its fixed lengths and ground contact, causing persistent buckling.
        const float groundDrapeAllowance = mHasGround
            ? std::max(0.f, mGroundZ - shapePositions.back().z()) : 0.f;

        for (int substep = 0; substep < substeps; ++substep)
        {
            // Pin the authored attachment region. For capes this keeps the
            // shoulder/upper-arm area from being dragged by the lower cloth.
            for (int i = 0; i < pinCount; ++i)
            {
                mPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
                mPreviousPositions[static_cast<std::size_t>(i)] = restPositions[static_cast<std::size_t>(i)];
            }

            // Soft attachment zone. Move current and previous positions together
            // so the tether changes position without generating fake velocity.
            const std::size_t softEnd = std::min(
                mPositions.size(), static_cast<std::size_t>(pinCount + softRootCount));
            for (std::size_t i = static_cast<std::size_t>(pinCount); i < softEnd; ++i)
            {
                const float followWeight = 1.f - std::pow(softRootDynamicWeight(i), strengthExponent);
                const osg::Vec3f correction = (shapePositions[i] - mPositions[i]) * followWeight;
                mPositions[i] += correction;
                mPreviousPositions[i] += correction;
            }

            // Apply lateral memory before integration/constraints so body and
            // ground collision always have final authority. This avoids a
            // frame-to-frame tug-of-war between shape preservation and contact.
            if (lateralMemory > 0.f)
            {
                for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                {
                    // Bip01 Head/Pelvis axes are not actor axes: in Siff local X
                    // is vertical. Preserve the horizontal plane perpendicular
                    // to gravity, leaving world-Z sag free.
                    const osg::Vec3f delta = shapePositions[i] - mPositions[i];
                    const osg::Vec3f correction(delta.x() * lateralFollow, delta.y() * lateralFollow, 0.f);
                    mPositions[i] += correction;
                    mPreviousPositions[i] += correction;
                }
            }

            for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
            {
                const osg::Vec3f current = mPositions[i];
                const float dynamicWeight = softRootDynamicWeight(i);
                const float stepRatio = mSettings.mStableTiming && mPreviousStep > 0.f
                    ? subDt / mPreviousStep : 1.f;
                osg::Vec3f velocity = (mPositions[i] - mPreviousPositions[i]) * stepRatio * friction
                    * std::pow(dynamicWeight, strengthExponent);

                if (stationary && velocityDeadzone > 0.f)
                {
                    const float displacementThreshold = velocityDeadzone * subDt;
                    if (velocity.length2() < displacementThreshold * displacementThreshold)
                        velocity.set(0.f, 0.f, 0.f);
                }

                const float phase
                    = static_cast<float>(simTime) * windFrequency + static_cast<float>(i) * 0.47f;
                osg::Vec3f acceleration(
                    std::sin(phase * 6.28318530718f) * effectiveWindStrength,
                    std::sin(phase * 4.117f + 0.9f) * effectiveWindStrength * 0.35f,
                    -gravity);
                acceleration += inertialAcceleration;
                if (airDrag > 0.f)
                {
                    // Frame carry preserves the mesh shell; air resistance is a
                    // separate bounded force opposing world motion. Include the
                    // relative particle velocity so it also damps free swinging.
                    osg::Vec3f drag = -(mFilteredRootVelocity + velocity / subDt) * airDrag;
                    const float limit = std::max(0.f, mSettings.mAirDragMaxAcceleration);
                    if (drag.length2() > limit * limit && drag.length2() > 0.f)
                    {
                        drag.normalize();
                        drag *= limit;
                    }
                    acceleration += drag;
                }

                osg::Vec3f step = velocity + acceleration * (subDt * subDt * dynamicWeight);
                if (step.length2() > maxStep * maxStep)
                {
                    step.normalize();
                    step *= maxStep;
                }

                mPreviousPositions[i] = current;
                mPositions[i] += step;
            }
            mPreviousStep = subDt;

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

                    const float restLength = mSettings.mStableTiming
                        ? (restPositions[i] - restPositions[i - 1]).length() : mSegmentLengths[i - 1];
                    const osg::Vec3f correction = delta * ((distance - restLength) / distance);

                    if (i == static_cast<std::size_t>(pinCount))
                    {
                        // The previous particle is pinned, so the first free
                        // particle takes the entire correction. Apply the same
                        // positional correction to history so it cannot become
                        // artificial velocity on the next substep.
                        mPositions[i] -= correction;
                        mPreviousPositions[i] -= correction;
                    }
                    else
                    {
                        const osg::Vec3f halfCorrection = correction * 0.5f;
                        mPositions[i - 1] += halfCorrection;
                        mPositions[i] -= halfCorrection;
                        mPreviousPositions[i - 1] += halfCorrection;
                        mPreviousPositions[i] -= halfCorrection;
                    }
                }

                if (maxLateralDeviation > 0.f)
                {
                    // A last-resort lateral guard keeps independent bone chains
                    // from exchanging sides/collapsing during abusive impulses.
                    // Vertical drape, ordinary swing, and contacts remain free.
                    for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                    {
                        // A guard must yield to an active contact. Clamping a
                        // particle back inside a capsule (or below the floor)
                        // makes the distance/contact constraints impossible.
                        bool inContact = mGroundContacts[i] != 0;
                        for (const auto& contacts : mBodyContacts)
                            inContact = inContact || contacts[i] != 0;
                        if (inContact)
                            continue;
                        osg::Vec3f lateral(mPositions[i].x() - shapePositions[i].x(),
                            mPositions[i].y() - shapePositions[i].y(), 0.f);
                        const float distance = lateral.length();
                        const float tipT = static_cast<float>(i - static_cast<std::size_t>(pinCount))
                            / static_cast<float>(std::max(1, static_cast<int>(mPositions.size()) - pinCount - 1));
                        const float allowedDeviation = maxLateralDeviation + groundDrapeAllowance * tipT * tipT;
                        if (distance <= allowedDeviation)
                            continue;
                        lateral /= distance;
                        const osg::Vec3f correctedWorld = mPositions[i] - lateral * (distance - allowedDeviation);
                        osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
                        const osg::Vec3f outward = lateral;
                        const float outwardVelocity = velocity * outward;
                        if (outwardVelocity > 0.f)
                            velocity -= outward * outwardVelocity;
                        mPositions[i] = correctedWorld;
                        mPreviousPositions[i] = correctedWorld - velocity;
                    }
                }
                if (bodyCollision)
                    solveBodyCollision(pinCount, bodyCollisionRadius, bodyCollisionMargin);
                solveGround(pinCount);
            }

            if (mSettings.mProjectVelocity)
            {
                // Remove axial velocity that distance constraints cannot permit;
                // carrying positional history alone would let gravity accumulate
                // an invisible speed in an already fully extended hanging chain.
                for (int pass = 0; pass < 6; ++pass)
                {
                    for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                    {
                        osg::Vec3f axis = mPositions[i] - mPositions[i - 1];
                        if (axis.normalize() <= 1e-5f)
                            continue;
                        const osg::Vec3f velocity = mPositions[i] - mPreviousPositions[i];
                        const osg::Vec3f parentVelocity = mPositions[i - 1] - mPreviousPositions[i - 1];
                        const osg::Vec3f axial = axis * ((velocity - parentVelocity) * axis);
                        if (i == static_cast<std::size_t>(pinCount))
                            mPreviousPositions[i] += axial;
                        else
                        {
                            mPreviousPositions[i] += axial * 0.5f;
                            mPreviousPositions[i - 1] -= axial * 0.5f;
                        }
                    }
                }
                // Contact normals have final authority over projected velocity.
                if (bodyCollision)
                    solveBodyCollision(pinCount, bodyCollisionRadius, bodyCollisionMargin);
                solveGround(pinCount);
            }

            // Kill only sub-millimetre-scale residual motion when the actor is
            // stationary. Real swinging/falling remains far above this threshold.
            if (stationary && velocityDeadzone > 0.f)
            {
                const float displacementThreshold = velocityDeadzone * subDt;
                const float threshold2 = displacementThreshold * displacementThreshold;
                for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                {
                    const osg::Vec3f residual = mPositions[i] - mPreviousPositions[i];
                    if (residual.length2() < threshold2)
                        mPreviousPositions[i] = mPositions[i];
                }
            }
        }

        if (canSleep)
        {
            float motion2 = 0.f;
            for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                motion2 = std::max(motion2, (mPositions[i] - frameStartPositions[i]).length2());
            const float threshold = mSettings.mSleepSpeed * static_cast<float>(frameDt);
            if (motion2 <= threshold * threshold)
                mSleepTime += static_cast<float>(frameDt);
            else
                mSleepTime = 0.f;
            const float sleepDelay = std::max(0.1f, mSettings.mSleepDelay);
            bool quietAmplitude = false;
            if (mSettings.mSleepAmplitude > 0.f)
            {
                if (mSleepMinPositions.size() != mPositions.size())
                {
                    mSleepMinPositions = mPositions;
                    mSleepMaxPositions = mPositions;
                }
                mSleepWindowTime += static_cast<float>(frameDt);
                for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        mSleepMinPositions[i][axis] = std::min(mSleepMinPositions[i][axis], mPositions[i][axis]);
                        mSleepMaxPositions[i][axis] = std::max(mSleepMaxPositions[i][axis], mPositions[i][axis]);
                    }
                if (mSleepWindowTime >= sleepDelay)
                {
                    quietAmplitude = true;
                    const float amplitude2 = mSettings.mSleepAmplitude * mSettings.mSleepAmplitude;
                    for (std::size_t i = static_cast<std::size_t>(pinCount); i < mPositions.size(); ++i)
                        quietAmplitude = quietAmplitude
                            && (mSleepMaxPositions[i] - mSleepMinPositions[i]).length2() <= amplitude2;
                    mSleepWindowTime = 0.f;
                    mSleepMinPositions = mPositions;
                    mSleepMaxPositions = mPositions;
                }
            }
            if (mSleepTime >= sleepDelay || quietAmplitude)
            {
                mSleeping = true;
                mPreviousPositions = mPositions;
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
                             << " softRootCount=" << softRootCount << " velocityDeadzone=" << velocityDeadzone
                             << " contactSlop=" << mSettings.mContactSlop
                             << " rotationCarry=" << rotationCarry << " lateralMemory=" << lateralMemory
                             << " inertia=" << inertia << " airDrag=" << airDrag
                             << " stableTiming=" << mSettings.mStableTiming
                             << " bodyCollision=" << bodyCollision << " bodyRadius=" << bodyCollisionRadius
                             << " globalOverride=" << useGlobal;
        }

        traverse(node, nv);
    }
}
