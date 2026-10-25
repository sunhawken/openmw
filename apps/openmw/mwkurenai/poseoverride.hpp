#ifndef OPENMW_MWKURENAI_POSEOVERRIDE_H
#define OPENMW_MWKURENAI_POSEOVERRIDE_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Node>
#include <osg/observer_ptr>
#include <osg/Vec3f>

#include <memory>
#include <string>
#include <vector>

namespace osg
{
    class MatrixTransform;
}

namespace MWKurenai
{
    // What the pose override should do this frame. Written by the driver (main thread), read by the callback in the
    // update traversal (main thread too).
    struct PoseTarget
    {
        bool active = false;
        // One entry per aim: Morrowind bone, the child whose direction is aimed, and the direction (character space:
        // x right, y forward, z up; zero = leave the bone alone).
        struct Aim
        {
            std::string bone;
            std::string child;
            osg::Vec3f direction;
            std::string sideChild; // empty: no twist
            osg::Vec3f side;
        };
        std::vector<Aim> aims;
    };

    // Re-aims bones of the player's skeleton like Red Ninja's skeleton, after Morrowind's own animation has posed
    // them this frame: each mapped bone turns (shortest arc) so the direction to its child matches the game's.
    // Attached to the root bone (Bip01): the bones' own controllers run first (traverse), then the override, and
    // all of it before the skinned meshes (siblings of the bone tree) take the bone matrices for this frame.
    class PoseOverride : public SceneUtil::NodeCallback<PoseOverride>
    {
    public:
        PoseOverride(std::shared_ptr<const PoseTarget> target, osg::Node* skeleton);
        void operator()(osg::Node* rootBone, osg::NodeVisitor* nv);
        void setTarget(std::shared_ptr<const PoseTarget> target) { mTarget = std::move(target); }

        // Diagnostics: bones found and turned in the last frame, and frames run.
        unsigned found() const { return mFound; }
        unsigned turned() const { return mTurned; }
        unsigned long frames() const { return mFrames; }
        float lastAngle() const { return mLastAngle; }
        const std::string& missing() const { return mMissing; }

    private:
        std::shared_ptr<const PoseTarget> mTarget;
        osg::observer_ptr<osg::Node> mSkeleton;
        unsigned mFound = 0, mTurned = 0;
        unsigned long mFrames = 0;
        float mLastAngle = 0;
        std::string mMissing; // names not found in the skeleton (last frame)
    };
}

#endif
