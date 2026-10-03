#include "skeleton.hpp"

#include <osg/MatrixTransform>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/lower.hpp>

#include <algorithm>
#include <cstdint>

namespace SceneUtil
{

    class InitBoneCacheVisitor : public osg::NodeVisitor
    {
    public:
        typedef std::vector<osg::MatrixTransform*> TransformPath;
        InitBoneCacheVisitor(std::unordered_map<std::string, TransformPath>& cache)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mCache(cache)
        {
        }

        void apply(osg::MatrixTransform& node) override
        {
            mPath.push_back(&node);
            mCache.emplace(Misc::StringUtils::lowerCase(node.getName()), mPath);
            traverse(node);
            mPath.pop_back();
        }

    private:
        TransformPath mPath;
        std::unordered_map<std::string, TransformPath>& mCache;
    };

    float Skeleton::sUpdateLodDistance = 0.f;

    void Skeleton::setUpdateLodDistance(float distance)
    {
        sUpdateLodDistance = std::max(0.f, distance);
    }

    Skeleton::Skeleton()
        : mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(Active)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Skeleton::Skeleton(const Skeleton& copy, const osg::CopyOp& copyop)
        : osg::Group(copy, copyop)
        , mBoneCacheInit(false)
        , mNeedToUpdateBoneMatrices(true)
        , mActive(copy.mActive)
        , mLastFrameNumber(0)
        , mLastCullFrameNumber(0)
    {
    }

    Bone* Skeleton::getBone(const std::string& name)
    {
        if (!mBoneCacheInit)
        {
            InitBoneCacheVisitor visitor(mBoneCache);
            accept(visitor);
            mBoneCacheInit = true;
        }

        BoneCache::iterator found = mBoneCache.find(Misc::StringUtils::lowerCase(name));
        if (found == mBoneCache.end())
            return nullptr;

        // find or insert in the bone hierarchy

        if (!mRootBone.get())
        {
            mRootBone = std::make_unique<Bone>();
        }

        Bone* bone = mRootBone.get();
        for (osg::MatrixTransform* matrixTransform : found->second)
        {
            const auto it = std::find_if(bone->mChildren.begin(), bone->mChildren.end(),
                [&](const auto& v) { return v->mNode.get() == matrixTransform; });

            if (it == bone->mChildren.end())
            {
                bone = bone->mChildren.emplace_back(std::make_unique<Bone>()).get();
                mNeedToUpdateBoneMatrices = true;
            }
            else
                bone = it->get();

            bone->mNode = matrixTransform;
        }

        return bone;
    }

    void Skeleton::updateBoneMatrices(unsigned int traversalNumber)
    {
        if (traversalNumber != mLastFrameNumber)
            mNeedToUpdateBoneMatrices = true;

        mLastFrameNumber = traversalNumber;

        if (mNeedToUpdateBoneMatrices)
        {
            if (mRootBone.get())
            {
                for (const auto& child : mRootBone->mChildren)
                    child->update(nullptr);
            }

            mNeedToUpdateBoneMatrices = false;
        }
    }

    void Skeleton::setActive(ActiveType active)
    {
        mActive = active;
        // When setting to Inactive, ensure we skip updates immediately
        // (traverse() checks mActive == Inactive && mLastFrameNumber != 0)
        if (active == Inactive && mLastFrameNumber == 0)
            mLastFrameNumber = 1;
    }

    bool Skeleton::getActive() const
    {
        return mActive != Inactive;
    }

    void Skeleton::markDirty()
    {
        mLastFrameNumber = 0;
        mBoneCache.clear();
        mBoneCacheInit = false;
    }

    void Skeleton::traverse(osg::NodeVisitor& nv)
    {
        const unsigned int frame = nv.getTraversalNumber();
        if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
        {
            if (mActive == Inactive && mLastFrameNumber != 0)
            {
                mUpdateSkippedFrame = frame;
                return;
            }
            if (mActive == SemiActive && mLastFrameNumber != 0)
            {
                if (mLastCullFrameNumber + 3 <= frame)
                {
                    mUpdateSkippedFrame = frame;
                    return;
                }
                // Distant actors animate at a reduced rate. Each skeleton gets its own phase so the work is spread
                // evenly over frames instead of every distant actor updating on the same frame.
                if (sUpdateLodDistance > 0.f && mLastCullDistance > sUpdateLodDistance)
                {
                    const unsigned int interval
                        = std::min(4u, 1u + static_cast<unsigned int>(mLastCullDistance / sUpdateLodDistance));
                    const unsigned int phase = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(this) >> 4);
                    if ((frame + phase) % interval != 0)
                    {
                        mUpdateSkippedFrame = frame;
                        return;
                    }
                }
            }
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            // Several cameras may cull us in one frame (main view, reflections, shadows); keep the nearest.
            const float distance = nv.getDistanceToViewPoint(osg::Vec3f(), true);
            if (mLastCullFrameNumber != frame || distance < mLastCullDistance)
                mLastCullDistance = distance;
            mLastCullFrameNumber = frame;
        }

        osg::Group::traverse(nv);
    }

    void Skeleton::childInserted(unsigned int)
    {
        markDirty();
    }

    void Skeleton::childRemoved(unsigned int, unsigned int)
    {
        markDirty();
    }

    Bone::Bone() {}

    void Bone::update(const osg::Matrixf* parentMatrixInSkeletonSpace)
    {
        // Lock the observed node; if it has been freed (a stale bone still lingering in the
        // hierarchy) skip it instead of dereferencing a dangling pointer, which used to crash.
        osg::ref_ptr<osg::MatrixTransform> node;
        if (!mNode.lock(node))
            return;

        if (parentMatrixInSkeletonSpace)
            mMatrixInSkeletonSpace = node->getMatrix() * (*parentMatrixInSkeletonSpace);
        else
            mMatrixInSkeletonSpace = node->getMatrix();

        for (const auto& child : mChildren)
            child->update(&mMatrixInSkeletonSpace);
    }

}
