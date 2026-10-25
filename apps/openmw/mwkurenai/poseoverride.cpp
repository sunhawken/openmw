#include "poseoverride.hpp"

#include <osg/ComputeBoundsVisitor>
#include <osg/MatrixTransform>
#include <osg/Quat>

#include <cmath>

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

    PoseOverride::PoseOverride(std::shared_ptr<const PoseTarget> target, osg::Node* skeleton)
        : mTarget(std::move(target))
        , mSkeleton(skeleton)
    {
    }

    void PoseOverride::operator()(osg::Node* rootBone, osg::NodeVisitor* nv)
    {
        traverse(rootBone, nv); // the bones' own animation controllers run first
        osg::ref_ptr<osg::Node> skeleton;
        if (!mSkeleton.lock(skeleton))
            return;
        ++mFrames;
        mFound = mTurned = 0;
        mMissing.clear();
        if (!mTarget || !mTarget->active)
            return;
        if (!mTarget->rootBone.empty())
            if (osg::MatrixTransform* bone = findBone(rootBone, mTarget->rootBone))
            {
                const osg::Matrixf boneS = toSkeleton(bone, skeleton.get());
                if (!mRootSet)
                {
                    mRootSet = true;
                    mRootRest = boneS.getTrans();
                    mRootFirst = mTarget->rootOffset;
                    mRootScale = mRootFirst.z() > 1.0f ? mRootRest.z() / mRootFirst.z() : 1.0f;
                }
                const osg::Vec3f want = mRootRest + (mTarget->rootOffset - mRootFirst) * mRootScale;
                const osg::Matrixf moved = boneS * osg::Matrixf::translate(want - boneS.getTrans());
                bone->setMatrix(moved * osg::Matrixf::inverse(boneS) * bone->getMatrix());
            }
        for (const PoseTarget::Aim& aim : mTarget->aims)
        {
            if (aim.direction.length2() < 1e-6f)
                continue;
            osg::MatrixTransform* bone = findBone(rootBone, aim.bone);
            osg::MatrixTransform* child = findBone(rootBone, aim.child);
            if (!bone || !child)
            {
                mMissing += (bone ? aim.child : aim.bone) + "; ";
                continue;
            }
            ++mFound;
            const osg::Matrixf boneS = toSkeleton(bone, skeleton.get());
            const osg::Vec3f origin = boneS.getTrans();
            osg::Vec3f current = toSkeleton(child, skeleton.get()).getTrans() - origin;
            if (current.normalize() < 1e-4f)
                continue;
            osg::Quat turn;
            turn.makeRotate(current, aim.direction);
            // Twist: about the aimed axis, so the side child's direction (across the axis) matches as well.
            if (!aim.sideChild.empty() && aim.side.length2() > 1e-6f)
                if (osg::MatrixTransform* sideBone = findBone(rootBone, aim.sideChild))
                {
                    const osg::Vec3f axis = aim.direction / aim.direction.length();
                    osg::Vec3f have = turn * (toSkeleton(sideBone, skeleton.get()).getTrans() - origin);
                    osg::Vec3f want = aim.side;
                    have -= axis * (have * axis);
                    want -= axis * (want * axis);
                    if (have.normalize() > 1e-4f && want.normalize() > 1e-4f)
                    {
                        const float angle = std::atan2((have ^ want) * axis, have * want);
                        turn = turn * osg::Quat(angle, axis);
                    }
                }
            // New skeleton-space transform: the old one turned about the bone's origin; back to the bone's local.
            const osg::Matrixf turned
                = boneS * osg::Matrixf::translate(-origin) * osg::Matrixf::rotate(turn) * osg::Matrixf::translate(origin);
            const osg::Matrixf local = turned * osg::Matrixf::inverse(boneS) * bone->getMatrix();
            bone->setMatrix(local);
            ++mTurned;
            mLastAngle = 2 * std::acos(std::min(1.0, std::abs(turn.w())));
        }
    }
}
