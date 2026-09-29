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
        // 0 = use the global "verlet pin count" setting.
        int mPinCount = 0;

        // Optional soft transition immediately below the pinned region.
        // The first free bones remain close to their authored pose and smoothly
        // become fully dynamic farther down the chain.
        int mSoftRootCount = 0;
        float mSoftRootStrength = 0.f;

        // Jitter suppression. Velocity is expressed in world units/second and
        // contact slop in world units. Positional constraints/collisions also
        // preserve pre-correction velocity so they do not inject fake energy.
        float mVelocityDeadzone = 0.6f;
        float mContactSlop = 0.08f;

        // Carry a fraction of the parent bone's frame rotation into the particles.
        // 0 = translation only, 1 = full parent-frame transform.
        float mRotationCarry = 0.f;

        // Actor-local XY shape memory. Preserves authored width/depth while leaving
        // vertical swing and chain-length motion free.
        float mLateralMemory = 0.f;

        // Also collide with thigh/calf capsules (skirts, long hair) and keep particles above the floor.
        bool mCollideLegs = false;
        bool mGround = false;
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
        void initializeBodyCollisionNodes(osg::MatrixTransform* node);
        void solveBodyCollision(int pinCount, float radius, float margin);
        void solveGround(int pinCount);

        std::vector<osg::ref_ptr<osg::MatrixTransform>> mChain;
        std::vector<osg::Matrix> mRestLocalMatrices;
        std::vector<osg::Vec3f> mPositions;
        std::vector<osg::Vec3f> mPreviousPositions;
        std::vector<float> mSegmentLengths;
        struct BodyCapsule
        {
            osg::ref_ptr<osg::MatrixTransform> mA;
            osg::ref_ptr<osg::MatrixTransform> mB;
            float mRadiusScale;
        };
        std::vector<BodyCapsule> mBodyCapsules;
        osg::ref_ptr<osg::Node> mGroundNode;
        bool mBodyCollisionInitialized = false;

        VerletClothSettings mSettings;
        osg::Vec3f mPreviousAnchor;
        osg::Matrix mPreviousRootParentWorld;
        bool mInitialized = false;
        double mLastSimTime = -1.0;
        bool mDebug = false;
        int mDebugCounter = 0;
    };
}

#endif
