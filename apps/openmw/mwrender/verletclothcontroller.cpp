#include "verletclothcontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/nifosg/matrixtransform.hpp>

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
        if ((anchor - mPreviousAnchor).length2() > sTeleportResetDistance * sTeleportResetDistance)
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

        const int substeps = std::clamp(mSettings.mSubsteps, 1, 8);
        const int iterations = std::clamp(mSettings.mIterations, 1, 32);
        const float subDt = static_cast<float>(dt / static_cast<double>(substeps));
        const float frictionBase = std::clamp(mSettings.mFriction, 0.f, 1.f);
        const float friction = std::pow(frictionBase, subDt * sReferenceFps);
        const float maxStep = std::max(0.01f, mSettings.mMaxStep);

        for (int substep = 0; substep < substeps; ++substep)
        {
            // Particle 0 is pinned to the animated skeleton attachment point.
            mPositions[0] = anchor;
            mPreviousPositions[0] = anchor;

            for (std::size_t i = 1; i < mPositions.size(); ++i)
            {
                const osg::Vec3f current = mPositions[i];
                osg::Vec3f velocity = (mPositions[i] - mPreviousPositions[i]) * friction;

                const float phase = static_cast<float>(simTime) * mSettings.mWindFrequency
                    + static_cast<float>(i) * 0.47f;
                const osg::Vec3f acceleration(
                    std::sin(phase * 6.28318530718f) * mSettings.mWindStrength,
                    std::sin(phase * 4.117f + 0.9f) * mSettings.mWindStrength * 0.35f,
                    -mSettings.mGravity);

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
                mPositions[0] = anchor;

                for (std::size_t i = 1; i < mPositions.size(); ++i)
                {
                    osg::Vec3f delta = mPositions[i] - mPositions[i - 1];
                    const float distance = delta.length();
                    if (distance <= 1e-5f)
                        continue;

                    const float restLength = mSegmentLengths[i - 1];
                    const osg::Vec3f correction = delta * ((distance - restLength) / distance);

                    if (i == 1)
                    {
                        // The root is pinned, so the second particle takes the
                        // entire correction.
                        mPositions[i] -= correction;
                    }
                    else
                    {
                        mPositions[i - 1] += correction * 0.5f;
                        mPositions[i] -= correction * 0.5f;
                    }
                }
            }
        }

        writeBoneTransforms(rootParentWorld);

        if (mDebug && (mDebugCounter++ % 60) == 0)
        {
            const float tipDistance = (mPositions.back() - restPositions.back()).length();
            Log(Debug::Info) << "Verlet cloth: root=" << node->getName() << " particles=" << mPositions.size()
                             << " tip displacement=" << tipDistance << " friction=" << frictionBase
                             << " gravity=" << mSettings.mGravity << " wind=" << mSettings.mWindStrength;
        }

        traverse(node, nv);
    }
}
