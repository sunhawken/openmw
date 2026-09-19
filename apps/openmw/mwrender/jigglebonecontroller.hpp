#ifndef OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H
#define OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Vec3f>

#include <string>

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
        /// @param debug when true, logs bone-lookup and per-frame displacement info
        /// (throttled). Controlled by the "jiggle bone debug" setting in [Game].
        /// @param isPlayer when true, this bone belongs to the player character. While the
        /// "jiggle player only" setting is on, the manual breast/butt Z-offset sliders (player-mesh
        /// -specific tuning) are applied only to the player's bones and skipped for NPCs.
        explicit JiggleBoneController(bool debug = false, bool isPlayer = false);

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    private:
        // The manual Z offset for this bone, or 0 for NPC bones (offsets are player-only).
        float zOffsetFor(const std::string& boneName) const;

        osg::Vec3f mSimWorldPos;
        osg::Vec3f mVelocity;
        osg::Matrix mRestLocalMatrix;
        bool mInitialized;
        double mLastSimTime;
        bool mDebug;
        bool mIsPlayer;
        int mDebugCounter = 0;
    };
}

#endif
