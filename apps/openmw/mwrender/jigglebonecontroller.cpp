#include "jigglebonecontroller.hpp"

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nifosg/matrixtransform.hpp>
#include <components/settings/values.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

#include <string>

namespace MWRender
{
    namespace
    {
        // dt is clamped regardless of settings, so a hitch/pause doesn't blow up the spring.
        constexpr double sMaxDeltaTime = 0.1;

        // Extra static world-space Z (up/down) nudge for this bone's rest position,
        // read from the live-tunable settings sliders (Physics tab) rather than baked
        // into the NIF - identified by bone name since a single controller instance
        // doesn't otherwise know if it's on a breast or butt bone.
        float manualZOffsetFor(const std::string& boneName)
        {
            // Case-insensitive: auto-rigged bones are named "Bip01 L Breast" while some
            // externally-rigged NIFs use lowercase "bip01 l breast".
            if (Misc::StringUtils::ciFind(boneName, "breast") != std::string::npos)
                return Settings::game().mJiggleBoneBreastZOffset;
            if (Misc::StringUtils::ciFind(boneName, "butt") != std::string::npos)
                return Settings::game().mJiggleBoneButtZOffset;
            return 0.f;
        }
    }

    JiggleBoneController::JiggleBoneController(bool debug)
        : mSimWorldPos(0, 0, 0)
        , mVelocity(0, 0, 0)
        , mInitialized(false)
        , mLastSimTime(-1.0)
        , mDebug(debug)
    {
    }

    void JiggleBoneController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        // Use the path owned by the traversal currently invoking this callback.
        // getParentalNodePaths() walks the mutable scene graph and may return a
        // stale path while actor parts are being rebuilt. In that case OSG can
        // dereference a transform that has already been detached. The visitor's
        // path is valid for this update traversal and therefore also tells us
        // whether this node is still attached to the graph being updated.
        if (!node || !nv)
            return;

        const osg::NodePath& traversalPath = nv->getNodePath();
        if (traversalPath.empty() || traversalPath.back() != node)
        {
            // A detached/replaced part can retain its callback until the old
            // subtree is released. Do not simulate against an unknown parent;
            // the replacement body gets a fresh controller during its rebuild.
            traverse(node, nv);
            return;
        }

        osg::NodePath parentPath = traversalPath;
        parentPath.pop_back();
        const osg::Matrix parentWorldMatrix = parentPath.empty() ? osg::Matrix::identity()
                                                                  : osg::computeLocalToWorld(parentPath);

        const double simTime = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;

        if (!mInitialized)
        {
            // Cache the true NIF bind-pose local matrix exactly once. We must not
            // re-read node->getMatrix() on later frames: this same function just
            // wrote the jiggled position into that matrix, so re-reading it would
            // mean "rest" drifts to wherever the spring last left off, collapsing
            // the restoring force to ~0 after the very first frame.
            mRestLocalMatrix = node->getMatrix();
            osg::Vec3f restWorldPos = mRestLocalMatrix.getTrans() * parentWorldMatrix;
            restWorldPos.z() += manualZOffsetFor(node->getName());
            mSimWorldPos = restWorldPos;
            mVelocity = osg::Vec3f(0, 0, 0);
            mInitialized = true;
            mLastSimTime = simTime;
            traverse(node, nv);
            return;
        }

        const osg::Vec3f restTranslation = mRestLocalMatrix.getTrans();
        osg::Vec3f restWorldPos = restTranslation * parentWorldMatrix;
        restWorldPos.z() += manualZOffsetFor(node->getName());

        // Live master off-switch: the "jiggle auto rig" toggle also freezes all jiggle motion.
        // While off, pin the bone to its rest pose and keep the sim parked at rest (tracking the
        // parent as it moves) and the clock current - so flipping it back on resumes seamlessly
        // in-game with no reload and no spring "kick". mInitialized stays true throughout.
        if (!Settings::game().mJiggleAutoRig)
        {
            mSimWorldPos = restWorldPos;
            mVelocity = osg::Vec3f(0, 0, 0);
            mLastSimTime = simTime;
            if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
                nifTransform->setTranslation(restTranslation);
            else
                node->setMatrix(mRestLocalMatrix);
            traverse(node, nv);
            return;
        }

        double dt = simTime - mLastSimTime;
        mLastSimTime = simTime;
        if (dt <= 0.0)
        {
            traverse(node, nv);
            return;
        }
        if (dt > sMaxDeltaTime)
            dt = sMaxDeltaTime;
        const float fdt = static_cast<float>(dt);

        const float stiffness = Settings::game().mJiggleBoneStiffness;
        const float damping = Settings::game().mJiggleBoneDamping;
        const float maxDisplacement = Settings::game().mJiggleBoneMaxDisplacement;
        const float intensity = Settings::game().mJiggleBoneIntensity;

        osg::Vec3f displacement = mSimWorldPos - restWorldPos;
        const osg::Vec3f acceleration = (displacement * -stiffness) + (mVelocity * -damping);
        mVelocity += acceleration * fdt;
        mSimWorldPos += mVelocity * fdt;

        displacement = mSimWorldPos - restWorldPos;
        if (displacement.length2() > maxDisplacement * maxDisplacement)
        {
            displacement.normalize();
            displacement *= maxDisplacement;
            mSimWorldPos = restWorldPos + displacement;
        }

        const osg::Vec3f visualWorldPos = restWorldPos + displacement * intensity;

        const osg::Matrix parentWorldInverse = osg::Matrix::inverse(parentWorldMatrix);
        const osg::Vec3f newLocalTranslation = visualWorldPos * parentWorldInverse;

        if (mDebug && (mDebugCounter++ % 60) == 0)
        {
            Log(Debug::Warning) << "Jiggle bone debug: node=" << node->getName() << " dt=" << dt
                                 << " restWorldPos=" << restWorldPos.x() << "," << restWorldPos.y() << ","
                                 << restWorldPos.z() << " simWorldPos=" << mSimWorldPos.x() << "," << mSimWorldPos.y()
                                 << "," << mSimWorldPos.z() << " displacementLen=" << displacement.length()
                                 << " newLocalTranslation=" << newLocalTranslation.x() << "," << newLocalTranslation.y()
                                 << "," << newLocalTranslation.z();
        }

        if (auto* nifTransform = dynamic_cast<NifOsg::MatrixTransform*>(node))
            nifTransform->setTranslation(newLocalTranslation);
        else
        {
            osg::Matrix newMatrix = mRestLocalMatrix;
            newMatrix.setTrans(newLocalTranslation);
            node->setMatrix(newMatrix);
        }

        traverse(node, nv);
    }
}
