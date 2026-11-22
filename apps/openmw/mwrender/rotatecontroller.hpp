#ifndef OPENMW_MWRENDER_ROTATECONTROLLER_H
#define OPENMW_MWRENDER_ROTATECONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>
#include <osg/Quat>

namespace osg
{
    class MatrixTransform;
}

namespace MWRender
{

    /// Applies a rotation in \a relativeTo's space.
    /// @note Assumes that the node being rotated has its "original" orientation set every frame by a different
    /// controller. The rotation is then applied on top of that orientation.
    class RotateController : public SceneUtil::NodeCallback<RotateController, osg::MatrixTransform*>
    {
    public:
        RotateController(osg::Node* relativeTo);

        void setEnabled(bool enabled);
        void setOffset(const osg::Vec3f& offset);
        void setRotate(const osg::Quat& rotate);
        void setRotationPivot(const osg::Vec3f& pivot)
        {
            mRotationPivot = pivot;
            mUseRotationPivot = true;
        }

        void setRotationCarrier(const osg::MatrixTransform* carrier) { mRotationCarrier = carrier; }

        const osg::Vec3f& getOffset() const { return mOffset; }

        const osg::Quat& getRotate() const { return mRotate; }

        osg::Node* getRelativeTo() const { return mRelativeTo; }

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    protected:
        bool mEnabled;
        osg::Vec3f mOffset;
        osg::Vec3f mRotationPivot;
        bool mUseRotationPivot = false;
        const osg::MatrixTransform* mRotationCarrier = nullptr;
        osg::Quat mRotate;
        osg::Node* mRelativeTo;
    };

}

#endif
