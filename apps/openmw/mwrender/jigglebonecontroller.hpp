#ifndef OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H
#define OPENMW_MWRENDER_JIGGLEBONECONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Node>
#include <osg/Vec3f>

#include <optional>
#include <string>
#include <utility>

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
        /// @param isPlayer when true, this bone belongs to the player character.
        /// @param actorName the NPC display name, matched against "jiggle npc enabled names".
        explicit JiggleBoneController(bool debug = false, bool isPlayer = false, std::string actorName = {});

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    private:
        // The breast/butt Z offset for this bone: the stored per-mesh tuning for this actor's body,
        // else the live slider value. The stored lookup parses setting strings, so it is cached and
        // redone only when Misc::JiggleZOffset::generation() changes.
        float zOffsetFor(const std::string& boneName);

        enum class BoneKind
        {
            Unknown,
            Other,
            Breast,
            Butt,
        };
        BoneKind mBoneKind = BoneKind::Unknown;
        unsigned mZGeneration = ~0u;
        std::optional<std::pair<float, float>> mStoredZ;
        // Reused every frame for the parent's node path, so the update does not allocate.
        osg::NodePath mParentPath;

        osg::Vec3f mSimWorldPos;
        osg::Vec3f mVelocity;
        osg::Vec3f mPreviousRestWorldPos;
        osg::Matrix mRestLocalMatrix;
        bool mInitialized;
        double mLastSimTime;
        bool mDebug;
        bool mIsPlayer;
        std::string mActorName;
        int mDebugCounter = 0;
    };
}

#endif
