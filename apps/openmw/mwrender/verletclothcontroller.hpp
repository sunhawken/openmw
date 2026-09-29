#ifndef OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H
#define OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <vector>

namespace osg
{
    class MatrixTransform;
}

namespace MWRender
{
    /// Runtime settings for a lightweight Verlet cloth chain.
    ///
    /// This ports the current/previous-position + distance-constraint technique
    /// used by Josephbakulikira/Cloth-Simulation-With-python---Verlet-Integration
    /// to a chain of skinned OpenMW bones. The first bone is pinned to its normal
    /// animated rest position; remaining bone origins are Verlet particles.
    struct VerletClothSettings
    {
        bool mEnabled = false;
        int mCount = 0;

        float mFriction = 0.95f;
        float mGravity = 1.5f;
        float mWindStrength = 8.f;
        float mWindFrequency = 0.8f;

        int mIterations = 8;
        int mSubsteps = 2;
        float mMaxStep = 12.f;
    };

    /// Drives a sequential bone chain using Verlet integration and fixed-distance
    /// constraints. Intended for capes, skirts, tails, long hair and similar
    /// secondary-motion meshes already skinned to an authored bone chain.
    class VerletClothController : public SceneUtil::NodeCallback<VerletClothController, osg::MatrixTransform*>
    {
    public:
        VerletClothController(
            std::vector<osg::ref_ptr<osg::MatrixTransform>> chain, VerletClothSettings settings, bool debug = false);

        void operator()(osg::MatrixTransform* node, osg::NodeVisitor* nv);

    private:
        void initialize(const osg::Matrix& rootParentWorld, double simTime);
        void resetToRest(const osg::Matrix& rootParentWorld, double simTime);
        std::vector<osg::Vec3f> restWorldPositions(const osg::Matrix& rootParentWorld) const;
        void writeBoneTransforms(const osg::Matrix& rootParentWorld);

        std::vector<osg::ref_ptr<osg::MatrixTransform>> mChain;
        std::vector<osg::Matrix> mRestLocalMatrices;
        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mPreviousPositions;
        std::vector<float> mSegmentLengths;

        VerletClothSettings mSettings;
        osg::Vec3f mPreviousAnchor;
        bool mInitialized = false;
        double mLastSimTime = -1.0;
        bool mDebug = false;
        int mDebugCounter = 0;
    };
}

#endif
