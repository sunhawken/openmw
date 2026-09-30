#ifndef OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H
#define OPENMW_MWRENDER_VERLETCLOTHCONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>

#include <osg/Matrix>
#include <osg/Vec3f>
#include <osg/ref_ptr>

#include <array>
#include <deque>
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

        // Horizontal shape memory. Head/Pelvis local XY is not reliably the
        // lateral plane; preserve width/depth perpendicular to world gravity.
        float mLateralMemory = 0.f;
        // 0 disables this emergency bound; ordinary secondary motion stays free.
        float mMaxLateralDeviation = 0.f;

        // Opt-in for rigs authored for timing-independent Siff stability.
        bool mStableTiming = false;
        bool mProjectVelocity = false;
        // Bone origins can lie inside a coarse body capsule even when the skinned
        // surface is outside it. Do not make the authored attachment impossible.
        bool mRestCollisionFit = false;
        // Orient skinned cross-sections with the coherent airflow frame,
        // preserving bind twist/scale instead of shearing translated bones.
        bool mAlignBones = false;

        // Bounded reaction to parent translation acceleration. Unlike a second
        // anchor-displacement force, this produces starts/stops/jump lag without
        // continually pulling a steadily moving chain into a flat plane.
        float mInertia = 0.f;
        float mInertiaMaxAcceleration = 1800.f;

        // Air resistance uses world velocity, so steady movement has a visible
        // trailing force as well as the start/stop acceleration response.
        float mAirDrag = 0.f;
        float mAirDragMaxAcceleration = 1500.f;
        // Bend the volume reference with airflow; rigid bind-pose guards would
        // otherwise suppress the very trailing movement produced by air drag.
        float mAirShapeResponse = 0.f;

        // 0 disables sleep. Settled cloth wakes on parent/collider movement.
        float mSleepSpeed = 0.f;
        float mSleepDelay = 0.6f;
        float mSleepAmplitude = 0.f;

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
        osg::Vec3f velocityAt(double time) const;
        void initialize(const osg::Matrix& rootParentWorld, double simTime);
        void resetToRest(const osg::Matrix& rootParentWorld, double simTime);
        std::vector<osg::Vec3f> restWorldPositions(const osg::Matrix& rootParentWorld) const;
        void writeBoneTransforms(const osg::Matrix& rootParentWorld);
        void initializeBodyCollisionNodes(osg::MatrixTransform* node);
        void updateCollisionWorld(const std::vector<osg::Vec3f>& restPositions, float radius, float margin);
        void solveBodyCollision(int pinCount, float radius, float margin);
        void solveGround(int pinCount);
        void solveGroundParticle(std::size_t i);

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
        struct WorldCapsule
        {
            osg::Vec3f mA;
            osg::Vec3f mB;
            float mRadiusScale;
            std::vector<float> mParticleRadii;
        };
        std::vector<WorldCapsule> mWorldCapsules;
        std::vector<osg::Vec3f> mGroundDrapeDirections;
        std::vector<std::vector<unsigned char>> mBodyContacts;
        std::vector<unsigned char> mGroundContacts;
        float mGroundZ = 0.f;
        bool mHasGround = false;
        osg::ref_ptr<osg::Node> mGroundNode;
        bool mBodyCollisionInitialized = false;

        VerletClothSettings mSettings;
        osg::Vec3f mPreviousAnchor;
        osg::Matrix mPreviousRootParentWorld;
        osg::Matrix mPreviousAirBend;
        std::vector<osg::Vec3f> mPreviousShape;
        float mFlutterPhase = 0.f;
        std::deque<std::array<float, 4>> mVelocityHistory;
        osg::Vec3f mPreviousRootVelocity;
        osg::Vec3f mFilteredRootAcceleration;
        osg::Vec3f mFilteredRootVelocity;
        float mPreviousStep = 0.f;
        bool mRootVelocityInitialized = false;
        bool mSleeping = false;
        float mSleepTime = 0.f;
        float mSleepWindowTime = 0.f;
        std::vector<osg::Vec3f> mSleepMinPositions;
        std::vector<osg::Vec3f> mSleepMaxPositions;
        bool mInitialized = false;
        double mLastSimTime = -1.0;
        bool mDebug = false;
        int mDebugCounter = 0;
    };
}

#endif
