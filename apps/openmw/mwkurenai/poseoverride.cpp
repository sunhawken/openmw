#include "poseoverride.hpp"

#include <osg/ComputeBoundsVisitor>
#include <osg/MatrixTransform>
#include <osg/Quat>

#include <components/misc/strings/lower.hpp>
#include <components/sceneutil/visitor.hpp>

namespace MWKurenai
{
    namespace
    {
        // The node's transform relative to the skeleton node (the actor's own space).
        osg::Matrixf toSkeleton(osg::Node* node, osg::Node* skeleton)
        {
            osg::Matrixf m;
            osg::Node* n = node;
            while (n && n != skeleton)
            {
                if (auto* t = dynamic_cast<osg::MatrixTransform*>(n))
                    m.postMult(t->getMatrix());
                n = n->getNumParents() ? n->getParent(0) : nullptr;
            }
            return m;
        }

        osg::MatrixTransform* findBone(osg::Node* skeleton, const std::string& name)
        {
            SceneUtil::FindByNameVisitor find(name);
            skeleton->accept(find);
            return dynamic_cast<osg::MatrixTransform*>(find.mFoundNode);
        }
    }

    PoseOverride::PoseOverride(std::shared_ptr<const PoseTarget> target)
        : mTarget(std::move(target))
    {
    }

    void PoseOverride::operator()(osg::Node* skeleton, osg::NodeVisitor* nv)
    {
        traverse(skeleton, nv); // the bones' own animation controllers run first
        if (!mTarget || !mTarget->active)
            return;
        for (const PoseTarget::Aim& aim : mTarget->aims)
        {
            if (aim.direction.length2() < 1e-6f)
                continue;
            osg::MatrixTransform* bone = findBone(skeleton, aim.bone);
            osg::MatrixTransform* child = findBone(skeleton, aim.child);
            if (!bone || !child)
                continue;
            const osg::Matrixf boneS = toSkeleton(bone, skeleton);
            const osg::Vec3f origin = boneS.getTrans();
            osg::Vec3f current = toSkeleton(child, skeleton).getTrans() - origin;
            if (current.normalize() < 1e-4f)
                continue;
            osg::Quat turn;
            turn.makeRotate(current, aim.direction);
            // New skeleton-space transform: the old one turned about the bone's origin; back to the bone's local.
            const osg::Matrixf turned
                = boneS * osg::Matrixf::translate(-origin) * osg::Matrixf::rotate(turn) * osg::Matrixf::translate(origin);
            const osg::Matrixf local = turned * osg::Matrixf::inverse(boneS) * bone->getMatrix();
            bone->setMatrix(local);
        }
    }
}
