#ifndef OPENMW_MWKURENAI_POSEOVERRIDE_H
#define OPENMW_MWKURENAI_POSEOVERRIDE_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Node>
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
        };
        std::vector<Aim> aims;
    };

    // Re-aims bones of the player's skeleton like Red Ninja's skeleton, after Morrowind's own animation has posed
    // them this frame: each mapped bone turns (shortest arc) so the direction to its child matches the game's.
    // Attached to the skeleton node; it lets the bones' own controllers run first, then overrides.
    class PoseOverride : public SceneUtil::NodeCallback<PoseOverride>
    {
    public:
        explicit PoseOverride(std::shared_ptr<const PoseTarget> target);
        void operator()(osg::Node* skeleton, osg::NodeVisitor* nv);
        void setTarget(std::shared_ptr<const PoseTarget> target) { mTarget = std::move(target); }

    private:
        std::shared_ptr<const PoseTarget> mTarget;
    };
}

#endif
