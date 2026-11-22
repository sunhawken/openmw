#ifndef OPENMW_COMPONENTS_SCENEUTIL_BODYPARTCOVERAGE_H
#define OPENMW_COMPONENTS_SCENEUTIL_BODYPARTCOVERAGE_H

#include <osg/ClipPlane>
#include <osg/Drawable>
#include <osg/Group>
#include <osg/NodeCallback>
#include <osgUtil/CullVisitor>

#include <cmath>
#include <mutex>
#include <optional>

namespace SceneUtil
{
    // Attach only to an instance-owned Group containing one direct rigid
    // Drawable. The plane is in that Drawable's coordinates, positive = kept.
    class BodyPartCoverage final : public osg::NodeCallback
    {
    public:
        BodyPartCoverage() = default;
        BodyPartCoverage(const BodyPartCoverage& copy, const osg::CopyOp& operation)
            : osg::NodeCallback(copy, operation)
        {
            // A cloned actor must obtain its own current garment plane.
        }
        META_Object(SceneUtil, BodyPartCoverage)

        bool setPlane(const osg::Vec4d& plane)
        {
            bool valid = std::hypot(plane.x(), plane.y(), plane.z()) > 1e-12;
            for (unsigned i = 0; i < 4; ++i) valid = valid && std::isfinite(plane[i]);
            std::lock_guard lock(mMutex);
            mPlane = valid ? std::optional(plane) : std::nullopt;
            return valid;
        }
        void clearPlane() { std::lock_guard lock(mMutex); mPlane.reset(); }
        std::optional<osg::Vec4d> getPlane() const { std::lock_guard lock(mMutex); return mPlane; }

        void operator()(osg::Node* node, osg::NodeVisitor* visitor) override
        {
            auto* cull = dynamic_cast<osgUtil::CullVisitor*>(visitor);
            const auto* group = node->asGroup();
            const auto plane = getPlane();
            if (!cull || !plane || !group || group->getNumChildren() != 1
                || !dynamic_cast<const osg::Drawable*>(group->getChild(0)))
            {
                traverse(node, visitor);
                return;
            }
            // glClipPlane captures the model-view when the state is applied.
            // A fresh attribute for EVERY visit prevents OSG's state cache from
            // retaining an eye-space plane across transforms, views or frames.
            // The StateGraph retains this immutable state through drawing.
            osg::ref_ptr<osg::StateSet> state = new osg::StateSet;
            state->setAttributeAndModes(new osg::ClipPlane(1, *plane),
                osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
            cull->pushStateSet(state);
            traverse(node, visitor);
            cull->popStateSet();
        }

    private:
        mutable std::mutex mMutex;
        std::optional<osg::Vec4d> mPlane;
    };
}

#endif
