#include "rotatecontroller.hpp"

#include <osg/MatrixTransform>
#include <osgAnimation/Bone>

namespace MWRender
{

    RotateController::RotateController(osg::Node* relativeTo)
        : mEnabled(true)
        , mRelativeTo(relativeTo)
    {
    }

    void RotateController::setEnabled(bool enabled)
    {
        mEnabled = enabled;
    }

    void RotateController::setRotate(const osg::Quat& rotate)
    {
        mRotate = rotate;
    }

    void RotateController::setOffset(const osg::Vec3f& offset)
    {
        mOffset = offset;
    }

    void RotateController::operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv)
    {
        if (!mEnabled)
        {
            traverse(node, nv);
            return;
        }
        osg::Matrix matrix = node->getMatrix();

        osg::Quat worldOrient;
        osg::Matrixf worldMat;
        osg::NodePathList nodepaths = node->getParentalNodePaths(mRelativeTo);

        if (!nodepaths.empty())
        {
            worldMat = osg::computeLocalToWorld(nodepaths[0]);
            worldOrient = worldMat.getRotate();
        }

        osg::Quat worldOrientInverse = worldOrient.inverse();

        if (mUseRotationPivot && !nodepaths.empty())
        {
            // First-person upper limbs aim about the stable eye rather than the
            // neck behind it, so looking up cannot swing the hands through the camera.
            const osg::Matrixf parentWorld = osg::Matrixf::inverse(matrix) * worldMat;
            // The carrier is an animated ancestor, so its callback has already run.
            // Its body/head translation remains outside the view-pitch rotation.
            osg::Vec3f pivot = mRotationPivot;
            if (mRotationCarrier)
                pivot += mRotationCarrier->getMatrix().getTrans();
            matrix = worldMat * osg::Matrixf::translate(-pivot) * osg::Matrixf::rotate(mRotate)
                * osg::Matrixf::translate(pivot) * osg::Matrixf::inverse(parentWorld);
        }
        else
        {
            osg::Quat orient = worldOrient * mRotate * worldOrientInverse * matrix.getRotate();
            matrix.setRotate(orient);
        }
        matrix.setTrans(matrix.getTrans() + worldOrientInverse * mOffset);

        node->setMatrix(matrix);

        // If we are linked to a bone we must call setMatrixInSkeletonSpace
        osgAnimation::Bone* b = dynamic_cast<osgAnimation::Bone*>(node);
        if (b)
        {
            osgAnimation::Bone* parent = b->getBoneParent();
            if (parent)
                matrix *= parent->getMatrixInSkeletonSpace();

            b->setMatrixInSkeletonSpace(matrix);
        }

        traverse(node, nv);
    }
}
