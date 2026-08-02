#ifndef OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H
#define OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Vec3f>

namespace osg
{
    class MatrixTransform;
}

namespace MWRender
{
    /// Applies a lightweight procedural spring-damper secondary-motion effect to a
    /// bone (e.g. breast/butt "jiggle bones" on body-replacer meshes), simulating
    /// physical lag as its parent bone moves. This is NOT rigid-body physics - no
    /// collision, no Jolt/Havok involvement - just a cheap per-frame calculation
    /// applied on top of whatever pose the bone's own animation already set this
    /// frame. Matches the same technique used by "jiggle bone"/CBP/HDT-style
    /// systems in other games.
    ///
    /// The bone is expected to have no animation controller of its own (it's a
    /// rigid, undriven child bone) - its NIF bind-pose local transform is treated
    /// as the "rest" position the spring continuously pulls back toward.
    class JiggleBoneController : public SceneUtil::NodeCallback<JiggleBoneController, osg::MatrixTransform*>
    {
    public:
        JiggleBoneController();

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    private:
        osg::Vec3f mSimWorldPos;
        osg::Vec3f mVelocity;
        osg::Matrix mRestLocalMatrix;
        bool mInitialized;
        double mLastSimTime;
    };
}

#endif
