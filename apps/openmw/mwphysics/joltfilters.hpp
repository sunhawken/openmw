#ifndef OPENMW_MWPHYSICS_JOLTFILTERS_H
#define OPENMW_MWPHYSICS_JOLTFILTERS_H

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>

namespace MWPhysics
{
    // Allows objects from a specific broad phase layer list only
    class MultiBroadPhaseLayerFilter : public JPH::BroadPhaseLayerFilter
    {
    public:
        explicit MultiBroadPhaseLayerFilter(std::vector<JPH::BroadPhaseLayer> layers)
            : mLayers(layers)
        {
        }

        virtual bool ShouldCollide(JPH::BroadPhaseLayer inLayer) const override
        {
            return std::find(mLayers.begin(), mLayers.end(), inLayer) != mLayers.end();
        }

    private:
        std::vector<JPH::BroadPhaseLayer> mLayers;
    };

    // Allows objects from a specific layer list only
    class MultiObjectLayerFilter : public JPH::ObjectLayerFilter
    {
    public:
        explicit MultiObjectLayerFilter(std::vector<JPH::ObjectLayer> layers)
            : mLayers(layers)
        {
        }

        virtual bool ShouldCollide(JPH::ObjectLayer inLayer) const override
        {
            return std::find(mLayers.begin(), mLayers.end(), inLayer) != mLayers.end();
        }

    private:
        std::vector<JPH::ObjectLayer> mLayers;
    };

    // Allows objects from a specific layer mask only
    class MaskedObjectLayerFilter : public JPH::ObjectLayerFilter
    {
    public:
        explicit MaskedObjectLayerFilter(int mask)
            : mMask(mask)
        {
            assert(mask > 0);
        }

        virtual bool ShouldCollide(JPH::ObjectLayer inLayer) const override
        {
            return (mMask & static_cast<int>(inLayer)) != 0;
        }

    private:
        int mMask;
    };

    class JoltTargetBodiesFilter : public JPH::BodyFilter
    {
    protected:
        std::vector<JPH::BodyID> mTargets;
        std::vector<JPH::BodyID> mIgnoreTargets;

    public:
        JoltTargetBodiesFilter() {}

        void PushTarget(const JPH::BodyID& body) { mTargets.push_back(body); }

        void IgnoreBody(const JPH::BodyID& inBodyID) { mIgnoreTargets.push_back(inBodyID); }

        void Clear()
        {
            mTargets.clear();
            mIgnoreTargets.clear();
        }

        virtual bool ShouldCollide(const JPH::BodyID& inBodyID) const override
        {
            // Ignore check
            if (std::find(mIgnoreTargets.begin(), mIgnoreTargets.end(), inBodyID) != mIgnoreTargets.end())
                return false;

            // Within targets list check, if not there dont collide
            if (!mTargets.empty())
            {
                if (std::find(mTargets.begin(), mTargets.end(), inBodyID) == mTargets.end())
                    return false;
            }

            return true;
        }
    };
}

#endif
