#include "jigglebonecontroller.hpp"

#include <components/nifosg/matrixtransform.hpp>

#include <osg/MatrixTransform>
#include <osg/NodeVisitor>

namespace MWRender
{
    namespace
    {
        // Tuning for the damped-spring model. Chosen to give a soft, slightly
        // lagged bounce without excessive wobble.
        constexpr float sStiffness = 100.0f; // higher = snappier/stiffer
        constexpr float sDamping = 10.0f; // higher = settles faster, less bounce
        constexpr float sMaxDisplacement = 3.0f; // clamp (game units) against runaway on a dt spike or teleport
        constexpr double sMaxDeltaTime = 0.1; // clamp dt so a hitch/pause doesn't blow up the spring
        // The raw spring displacement is subtle at this scale, so amplify the
        // rendered offset a bit without affecting the underlying simulation.
        constexpr float sVisualAmplification = 2.5f;
    }

    JiggleBoneController::JiggleBoneController()
        : mSimWorldPos(0, 0, 0)
        , mVelocity(0, 0, 0)
        , mInitialized(false)
        , mLastSimTime(-1.0)
    {
    }

    void JiggleBoneController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        // getParentalNodePaths() includes `node` itself as the path's last element;
        // drop it so we get the PARENT's world transform, since the offset needs to
        // be computed in the parent's space, not the bone's own (about to be modified).
        osg::Matrix parentWorldMatrix;
        osg::NodePathList nodePaths = node->getParentalNodePaths();
        if (!nodePaths.empty() && nodePaths[0].size() > 1)
        {
            osg::NodePath parentPath = nodePaths[0];
            parentPath.pop_back();
            parentWorldMatrix = osg::computeLocalToWorld(parentPath);
        }

        const double simTime = nv->getFrameStamp() ? nv->getFrameStamp()->getSimulationTime() : 0.0;

        if (!mInitialized)
        {
            // Cache the true NIF bind-pose local matrix exactly once. We must not
            // re-read node->getMatrix() on later frames: this same function just
            // wrote the jiggled position into that matrix, so re-reading it would
            // mean "rest" drifts to wherever the spring last left off, collapsing
            // the restoring force to ~0 after the very first frame.
            mRestLocalMatrix = node->getMatrix();
            const osg::Vec3f restWorldPos = mRestLocalMatrix.getTrans() * parentWorldMatrix;
            mSimWorldPos = restWorldPos;
            mVelocity = osg::Vec3f(0, 0, 0);
            mInitialized = true;
            mLastSimTime = simTime;
            traverse(node, nv);
            return;
        }

        const osg::Vec3f restTranslation = mRestLocalMatrix.getTrans();
        const osg::Vec3f restWorldPos = restTranslation * parentWorldMatrix;

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

        osg::Vec3f displacement = mSimWorldPos - restWorldPos;
        const osg::Vec3f acceleration = (displacement * -sStiffness) + (mVelocity * -sDamping);
        mVelocity += acceleration * fdt;
        mSimWorldPos += mVelocity * fdt;

        displacement = mSimWorldPos - restWorldPos;
        if (displacement.length2() > sMaxDisplacement * sMaxDisplacement)
        {
            displacement.normalize();
            displacement *= sMaxDisplacement;
            mSimWorldPos = restWorldPos + displacement;
        }

        const osg::Vec3f visualWorldPos = restWorldPos + displacement * sVisualAmplification;

        const osg::Matrix parentWorldInverse = osg::Matrix::inverse(parentWorldMatrix);
        const osg::Vec3f newLocalTranslation = visualWorldPos * parentWorldInverse;

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
